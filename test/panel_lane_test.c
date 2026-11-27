/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native panel (panel-app) host gauntlet:
 *  - PANEL-8 hold-to-confirm invariants (lane.c)
 *  - Full host-simulated panel-app state machines (Pad, VideoOut, IME, virtual clock,
 *    loopback HTTP, real PNG artwork decoding via stb_image)
 *  - Deterministic scenarios covering nav, initial jump, L1/R1 sections, L2 wheel
 *    (left & right sticks + release without selection), Circle, Triangle/Detail,
 *    Search/IME (UTF-8, surrogate pairs, cancel, init fail), Settings, Downloads/URL,
 *    full/short/cancelled holds, empty catalog, network errors & recovery, concurrent
 *    refresh mid-hold, and clean exit with flip drain
 *  - Bounded deterministic input fuzzing with per-tick state invariants
 *  - Objective performance metrics benchmark (printed, not asserted) */
#include "../panel-app/lane.h"
#include "panel_host.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                   \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---------------- Fixture Catalogs ---------------- */

static const char *k_catalog_mixed =
    "{\"version\":\"1.0.0-test\",\"lang\":\"en\",\"setup\":false,"
    "\"drives\":[{\"label\":\"USB0\",\"free\":107374182400}],"
    "\"games\":["
    "{\"title_id\":\"CUSA00001\",\"name\":\"Alpha Quest\",\"state\":\"ready\",\"size\":1000000000,\"drive\":\"USB0\"},"
    "{\"title_id\":\"CUSA00002\",\"name\":\"Beta Racer\",\"state\":\"removed\",\"size\":2000000000,\"drive\":\"USB0\"},"
    "{\"title_id\":\"CUSA00003\",\"name\":\"Charlie Noir\",\"state\":\"offline\",\"size\":3000000000,\"drive\":\"USB0\"},"
    "{\"title_id\":\"CUSA00004\",\"name\":\"Delta Force\",\"state\":\"running\",\"size\":4000000000,\"drive\":\"USB0\"},"
    "{\"title_id\":\"CUSA00005\",\"name\":\"Echo Drift\",\"state\":\"restart\",\"size\":5000000000,\"drive\":\"USB0\"},"
    "{\"title_id\":\"CUSA00006\",\"name\":\"Foxtrot Zero\",\"state\":\"ready\",\"size\":6000000000,\"drive\":\"USB0\",\"update\":\"01.02\"}"
    "]}";

static const char *k_catalog_shifted =
    "{\"version\":\"1.0.0-test\",\"lang\":\"en\",\"setup\":false,"
    "\"drives\":[{\"label\":\"USB0\",\"free\":107374182400}],"
    "\"games\":["
    "{\"title_id\":\"CUSA00006\",\"name\":\"AAA Replacement\",\"state\":\"ready\",\"size\":6000000000,\"drive\":\"USB0\"},"
    "{\"title_id\":\"CUSA00002\",\"name\":\"Beta Racer\",\"state\":\"removed\",\"size\":2000000000,\"drive\":\"USB0\"}"
    "]}";

static const char *k_catalog_empty =
    "{\"version\":\"1.0.0-test\",\"lang\":\"en\",\"setup\":false,\"drives\":[],\"games\":[]}";

static const char *k_catalog_20_prefix =
    "{\"version\":\"1.0.0-test\",\"lang\":\"en\",\"setup\":false,"
    "\"drives\":[{\"label\":\"USB0\",\"free\":500000000000}],\"games\":[";

static char g_catalog_20[8192];

static void build_catalog_20(void) {
    size_t pos = (size_t)snprintf(g_catalog_20, sizeof(g_catalog_20), "%s", k_catalog_20_prefix);
    for (int i = 0; i < 20; i++) {
        const char *st = (i % 4 == 0) ? "ready" : (i % 4 == 1) ? "removed" : (i % 4 == 2) ? "offline" : "ready";
        pos += (size_t)snprintf(g_catalog_20 + pos, sizeof(g_catalog_20) - pos,
                                "%s{\"title_id\":\"CUSA%05d\",\"name\":\"%c Game %02d\",\"state\":\"%s\","
                                "\"size\":%lld,\"drive\":\"USB0\"}",
                                i ? "," : "", 100 + i, 'A' + (i % 20), i, st, (long long)(i + 1) * 1000000000LL);
    }
    snprintf(g_catalog_20 + pos, sizeof(g_catalog_20) - pos, "]}");
}

static uint8_t *g_png_fixture;
static size_t g_png_len;

/* ---------------- Helper: Balanced Lifecycle Check ---------------- */

static void check_lifecycle(const panel_lifecycle_t *l, const char *name) {
    CHECK(l->pad_opens == 1 && l->pad_closes == 1, "%s: balanced pad open/close (%d/%d)", name, l->pad_opens,
          l->pad_closes);
    CHECK(l->video_opens == 1 && l->video_closes == 1, "%s: balanced video open/close (%d/%d)", name, l->video_opens,
          l->video_closes);
    CHECK(l->video_reg == 1 && l->video_unreg == 1, "%s: balanced video buffers (%d/%d)", name, l->video_reg,
          l->video_unreg);
    CHECK(l->dm_allocs == 1 && l->dm_releases == 1, "%s: balanced direct memory (%d/%d)", name, l->dm_allocs,
          l->dm_releases);
    CHECK(l->munmaps == 1, "%s: the direct memory mapping is unmapped (%d)", name, l->munmaps);
    CHECK(l->ime_inits == l->ime_terms, "%s: balanced IME init/term (%d/%d)", name, l->ime_inits, l->ime_terms);
    CHECK(l->load_exec_calls > 0, "%s: the title leaves through sceSystemServiceLoadExec", name);
    CHECK(l->splash_hidden == 1, "%s: splash screen hidden", name);
}

/* ---------------- 1. Unit tests for lane.c (PANEL-8 + sections/jumps/wheel) ---------------- */

static void test_lane_unit(void) {
    const game ready = {.id = "DEM000001", .state = READY};
    const game other = {.id = "DEM000002", .state = READY};
    const game running = {.id = "DEM000001", .state = RUNNING};
    hold h;

    /* PANEL-8: Released early: nothing, and the same press cannot finish later. */
    hold_press(&h, REMOVE, &ready, 1000);
    CHECK(hold_tick(&h, &ready, 1, 1, 1000 + HOLD_MS - 1) == NONE, "short hold before HOLD_MS");
    CHECK(hold_tick(&h, &ready, 1, 0, 1000 + HOLD_MS - 1) == NONE, "released early");
    CHECK(hold_tick(&h, &ready, 1, 1, 1000 + 5 * HOLD_MS) == NONE, "cannot finish after release");

    /* PANEL-8: Held: once, however long the button stays down. */
    hold_press(&h, REMOVE, &ready, 1000);
    CHECK(hold_tick(&h, &ready, 1, 1, 1000 + HOLD_MS) == REMOVE, "fires at HOLD_MS");
    CHECK(hold_tick(&h, &ready, 1, 1, 1000 + 3 * HOLD_MS) == NONE, "fires only once");

    /* PANEL-8: Interruption mid-hold permanently drops hold even if original game returns. */
    const struct {
        const char *what;
        const game *focused;
        int online;
    } interrupted[] = {
        {"a refresh moved another game under the button", &other, 1},
        {"the library is empty", NULL, 1},
        {"the game started", &running, 1},
        {"the connection was lost", &ready, 0},
    };
    for (size_t i = 0; i < sizeof(interrupted) / sizeof(*interrupted); i++) {
        hold_press(&h, RESTORE, &ready, 1000);
        enum action during = hold_tick(&h, interrupted[i].focused, interrupted[i].online, 1, 1100);
        enum action after = hold_tick(&h, &ready, 1, 1, 1000 + HOLD_MS);
        CHECK(during == NONE && after == NONE, "acted although %s", interrupted[i].what);
    }

    /* Wheel geometry: deadzone returns -1, all 6 clockwise sectors map to 0..5. */
    CHECK(wheel_point(0, 0) == -1, "wheel deadzone center");
    CHECK(wheel_point(10, 10) == -1, "wheel deadzone small");
    CHECK(wheel_point(0, -100) == 0, "wheel top = scan (0)");
    CHECK(wheel_point(95, -55) == 1, "wheel top-right = send (1)");
    CHECK(wheel_point(95, 55) == 2, "wheel bottom-right = transfers (2)");
    CHECK(wheel_point(0, 100) == 3, "wheel bottom = settings (3)");
    CHECK(wheel_point(-95, 55) == 4, "wheel bottom-left = search (4)");
    CHECK(wheel_point(-95, -55) == 5, "wheel top-left = exit (5)");

    puts("PANEL-8 a hold acts only on the same game, in full, once");
}

/* ---------------- 2. Deterministic Scenarios ---------------- */

/* Helper: wait until worker finishes current task if active, with zero sleep. */
static int wait_if_busy(const panel_host_snapshot_t *snap) {
    if (snap->active_task) {
        panel_host_wait_worker_reply();
        return 1;
    }
    return 0;
}

typedef struct {
    int step;
    int saw_sections[SECTIONS];
    int saw_detail;
    int saw_wheel_left;
    int saw_wheel_right;
    int saw_release_no_sel;
    int saw_jump_fwd;
    int saw_jump_back;
    int saw_art;
} nav_ctx_t;

static void scenario_nav_sections_wheel(const panel_host_snapshot_t *snap, OrbisPadData *pad, int *fq, void *vctx) {
    nav_ctx_t *c = vctx;
    if (wait_if_busy(snap))
        return;

    switch (c->step) {
    case 0: /* Wait for initial LOAD */
        if (!snap->online || snap->visible_count != 6)
            return;
        c->saw_sections[SECTION_ALL] = snap->visible_count;
        /* Step down once */
        pad->buttons = ORBIS_PAD_BUTTON_DOWN;
        c->step++;
        break;
    case 1:
        CHECK(snap->selected == 1 && !strcmp(snap->focused_id, "CUSA00002"), "DOWN moved to Beta Racer (got %d %s)",
              snap->selected, snap->focused_id);
        /* Jump right by initial (B -> C) */
        pad->buttons = ORBIS_PAD_BUTTON_RIGHT;
        c->step++;
        break;
    case 2:
        CHECK(snap->selected == 2 && !strcmp(snap->focused_id, "CUSA00003"), "RIGHT jumped to Charlie (got %d)",
              snap->selected);
        c->saw_jump_fwd = 1;
        /* Jump left by initial (C -> B) */
        pad->buttons = ORBIS_PAD_BUTTON_LEFT;
        c->step++;
        break;
    case 3:
        CHECK(snap->selected == 1, "LEFT jumped back to Beta (got %d)", snap->selected);
        c->saw_jump_back = 1;
        /* Open Detail via Triangle */
        pad->buttons = ORBIS_PAD_BUTTON_TRIANGLE;
        c->step++;
        break;
    case 4:
        CHECK(snap->page == DETAIL, "Triangle opened DETAIL (got %d)", snap->page);
        c->saw_detail = 1;
        /* Close Detail via Circle */
        pad->buttons = ORBIS_PAD_BUTTON_CIRCLE;
        c->step++;
        break;
    case 5:
        CHECK(snap->page == LIBRARY, "Circle returned to LIBRARY");
        /* Cycle R1 across all sections (with 1-frame button release between repeats) */
        pad->buttons = ORBIS_PAD_BUTTON_R1;
        c->step++;
        break;
    case 6:
        pad->buttons = 0; /* release R1 */
        c->step++;
        break;
    case 7: /* SECTION_RUNNING (Delta Force -> 1) */
        CHECK(snap->section == SECTION_RUNNING && snap->visible_count == 1, "R1 -> RUNNING count=%d",
              snap->visible_count);
        c->saw_sections[SECTION_RUNNING] = snap->visible_count;
        pad->buttons = ORBIS_PAD_BUTTON_R1;
        c->step++;
        break;
    case 8:
        pad->buttons = 0; /* release R1 */
        c->step++;
        break;
    case 9: /* SECTION_NEEDS (offline + restart: CUSA00003, CUSA00005 -> 2) */
        CHECK(snap->section == SECTION_NEEDS && snap->visible_count == 2, "R1 -> NEEDS count=%d",
              snap->visible_count);
        c->saw_sections[SECTION_NEEDS] = snap->visible_count;
        /* L1 back to RUNNING */
        pad->buttons = ORBIS_PAD_BUTTON_L1;
        c->step++;
        break;
    case 10:
        pad->buttons = 0; /* release L1 */
        c->step++;
        break;
    case 11:
        CHECK(snap->section == SECTION_RUNNING, "L1 -> RUNNING");
        /* L1 back to ALL */
        pad->buttons = ORBIS_PAD_BUTTON_L1;
        c->step++;
        break;
    case 12:
        pad->buttons = 0; /* release L1 */
        c->step++;
        break;
    case 13:
        CHECK(snap->section == SECTION_ALL && snap->visible_count == 6, "L1 back to ALL");
        /* Open L2 wheel and release with NO selection */
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        c->step++;
        break;
    case 14:
        CHECK(snap->wheel == 1 && snap->wheel_at == -1, "L2 opened wheel unselected");
        /* Release L2 with sticks centered */
        pad->buttons = 0;
        c->step++;
        break;
    case 15:
        CHECK(snap->wheel == 0 && snap->page == LIBRARY && !snap->quit,
              "L2 release without selection kept LIBRARY (page=%d quit=%d)", snap->page, snap->quit);
        c->saw_release_no_sel = 1;
        /* Open L2 wheel, select sector 0 with LEFT stick, then sector 3 with RIGHT stick */
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        c->step++;
        break;
    case 16:
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        pad->leftStick.x = 128;
        pad->leftStick.y = 20; /* up -> sector 0 */
        c->step++;
        break;
    case 17:
        CHECK(snap->wheel == 1 && snap->wheel_at == 0, "left stick selected wheel 0 (got %d)", snap->wheel_at);
        c->saw_wheel_left = 1;
        /* Center left stick, point RIGHT stick down -> sector 3 */
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        pad->rightStick.x = 128;
        pad->rightStick.y = 235;
        c->step++;
        break;
    case 18:
        CHECK(snap->wheel == 1 && snap->wheel_at == 3, "right stick selected wheel 3 (got %d)", snap->wheel_at);
        c->saw_wheel_right = 1;
        /* Cancel wheel with Circle while still holding L2 */
        pad->buttons = ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_CIRCLE;
        c->step++;
        break;
    case 19:
        CHECK(snap->wheel == 0 && snap->page == LIBRARY, "Circle closed wheel without running slot 3");
        pad->buttons = 0; /* release L2 + Circle */
        c->step++;
        break;
    case 20:
        /* Wait for at least one real artwork decode from /icon/<tid> */
        c->saw_art = snap->art_cached > 0;
        /* Exit cleanly via L2 wheel slot 5 (top-left) */
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        c->step++;
        break;
    case 21:
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        pad->leftStick.x = 30;
        pad->leftStick.y = 70; /* top-left -> sector 5 (Exit) */
        c->step++;
        break;
    case 22:
        CHECK(snap->wheel == 1 && snap->wheel_at == 5, "wheel slot 5 (Exit) armed");
        pad->buttons = 0; /* release L2 -> executes slot 5 -> quit = 1 */
        c->step++;
        break;
    default:
        *fq = 1;
        break;
    }
}

typedef struct {
    int step;
    int hold_frames;
    int64_t hold_since; /* a held button is timed on the virtual clock: the loop sleeps 8 ms per frame while holding */
    int saw_short_no_action;
    int saw_cancel_wheel_no_action;
    int saw_full_remove;
    int saw_full_add;
    int saw_concurrent_drop;
} holds_ctx_t;

static void scenario_holds_and_concurrent_refresh(const panel_host_snapshot_t *snap, OrbisPadData *pad, int *fq,
                                                  void *vctx) {
    holds_ctx_t *c = vctx;
    /* Note: during step 6..8 we intentionally do NOT call wait_if_busy because /api/games
     * is paused on the mock server barrier while Cross is held! */
    if (c->step < 6 || c->step > 8) {
        if (wait_if_busy(snap))
            return;
    }

    switch (c->step) {
    case 0:
        if (!snap->online || snap->visible_count != 6)
            return;
        /* Short hold (200ms < 700ms) on CUSA00001 (READY -> REMOVE) */
        pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        c->hold_frames = 0;
        c->step++;
        break;
    case 1:
        c->hold_frames++;
        if (c->hold_frames < 12) { /* ~192ms */
            pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        } else {
            pad->buttons = 0; /* release early */
            c->step++;
        }
        break;
    case 2:
        CHECK(!panel_host_server_has_req("POST", "/api/unmount", NULL), "short hold did not trigger unmount");
        CHECK(snap->held_action == NONE, "hold cleared on release");
        c->saw_short_no_action = 1;
        /* Start hold, then cancel mid-hold with L2 wheel */
        pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        c->step++;
        break;
    case 3:
        CHECK(snap->held_action == REMOVE, "hold started");
        pad->buttons = ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_L2;
        c->step++;
        break;
    case 4:
        CHECK(snap->wheel == 1 && snap->held_action == NONE, "L2 wheel cancelled hold");
        c->saw_cancel_wheel_no_action = 1;
        pad->buttons = 0; /* close wheel unselected */
        c->step++;
        break;
    case 5:
        /* Concurrent refresh mid-hold: arm server barrier on /api/games and fast-forward
         * virtual clock past library_at (45s) so schedule() fires background LOAD! */
        panel_host_server_arm_games_barrier();
        panel_host_advance_clock_ms(46000);
        c->step++;
        break;
    case 6:
        /* Wait until background worker actually entered /api/games on server */
        panel_host_server_wait_games_arrived();
        /* Press and hold Cross on CUSA00001 while refresh is in-flight */
        pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        c->hold_since = panel_host_clock_ms();
        c->step++;
        break;
    case 7:
        CHECK(snap->held_action == REMOVE && !strcmp(snap->focused_id, "CUSA00001"),
              "holding REMOVE on CUSA00001 while refresh in-flight");
        /* Release server barrier with shifted catalog where CUSA00006 is now index 0! */
        panel_host_server_release_games_barrier(k_catalog_shifted);
        panel_host_wait_worker_reply();
        pad->buttons = ORBIS_PAD_BUTTON_CROSS; /* keep holding across reply */
        c->hold_since = panel_host_clock_ms();
        c->step++;
        break;
    case 8:
        /* Keep holding for > 700ms after catalog shifted to verify hold stays dropped! */
        if (panel_host_clock_ms() - c->hold_since < 800) {
            pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        } else {
            CHECK(!strcmp(snap->focused_id, "CUSA00006"), "focused shifted to CUSA00006");
            CHECK(!panel_host_server_has_req("POST", "/api/unmount", NULL),
                  "concurrent refresh mid-hold prevented wrong-game unmount");
            c->saw_concurrent_drop = 1;
            pad->buttons = 0;
            c->step++;
        }
        break;
    case 9:
        /* Now perform a clean FULL hold (>700ms) on CUSA00006 (READY -> REMOVE) */
        pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        c->hold_since = panel_host_clock_ms();
        c->step++;
        break;
    case 10:
        if (panel_host_clock_ms() - c->hold_since < 800) {
            pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        } else {
            pad->buttons = 0;
            c->step++;
        }
        break;
    case 11:
        CHECK(panel_host_server_has_req("POST", "/api/unmount?title_id=CUSA00006", NULL),
              "full hold executed /api/unmount?title_id=CUSA00006");
        c->saw_full_remove = 1;
        /* Move down to CUSA00002 (REMOVED -> ADD) and do full hold */
        pad->buttons = ORBIS_PAD_BUTTON_DOWN;
        c->step++;
        break;
    case 12:
        CHECK(!strcmp(snap->focused_id, "CUSA00002"), "moved to CUSA00002");
        pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        c->hold_since = panel_host_clock_ms();
        c->step++;
        break;
    case 13:
        if (panel_host_clock_ms() - c->hold_since < 800) {
            pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        } else {
            pad->buttons = 0;
            c->step++;
        }
        break;
    case 14:
        CHECK(panel_host_server_has_req("POST", "/api/mount?title_id=CUSA00002", NULL),
              "full hold executed /api/mount?title_id=CUSA00002");
        c->saw_full_add = 1;
        pad->buttons = ORBIS_PAD_BUTTON_CIRCLE; /* clean quit */
        c->step++;
        break;
    default:
        *fq = 1;
        break;
    }
}

typedef struct {
    int step;
    int sub;
    int saw_search_filter;
    int saw_ime_cancel;
    int saw_ime_fail;
    int saw_settings_saved;
    int saw_url_rejected;
    int saw_url_enqueued;
    int saw_phone_ip;
} ime_settings_ctx_t;

static void scenario_ime_settings_downloads(const panel_host_snapshot_t *snap, OrbisPadData *pad, int *fq, void *vctx) {
    ime_settings_ctx_t *c = vctx;
    if (wait_if_busy(snap))
        return;

    switch (c->step) {
    case 0:
        if (!snap->online || snap->visible_count != 6)
            return;
        /* Script IME to enter "Charlie" (tests Search via L2 wheel slot 4) */
        panel_host_set_ime_script(0, 2, ORBIS_DIALOG_OK, L"Charlie");
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        c->step++;
        break;
    case 1:
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        pad->leftStick.x = 30;
        pad->leftStick.y = 185; /* bottom-left -> sector 4 (Search) */
        c->step++;
        break;
    case 2:
        pad->buttons = 0; /* release -> starts IME */
        c->step++;
        break;
    case 3:
        if (snap->ime)
            return; /* wait for scripted IME frames to finish */
        CHECK(!strcmp(snap->query, "Charlie") && snap->visible_count == 1,
              "IME search filtered to Charlie (query='%s' visible=%d)", snap->query, snap->visible_count);
        c->saw_search_filter = 1;
        panel_host_advance_clock_ms(250); /* the pad is ignored for 220 ms after the keyboard closes */
        /* Test IME CANCELED preserves existing query */
        panel_host_set_ime_script(0, 1, ORBIS_DIALOG_CANCELED, L"Ignored");
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        c->step++;
        break;
    case 4:
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        pad->leftStick.x = 30;
        pad->leftStick.y = 185;
        c->step++;
        break;
    case 5:
        pad->buttons = 0;
        c->step++;
        break;
    case 6:
        if (snap->ime)
            return;
        CHECK(!strcmp(snap->query, "Charlie"), "canceled IME kept query='%s'", snap->query);
        c->saw_ime_cancel = 1;
        panel_host_advance_clock_ms(250);
        /* Circle clears query (does NOT quit!) */
        pad->buttons = ORBIS_PAD_BUTTON_CIRCLE;
        c->step++;
        break;
    case 7:
        CHECK(snap->query[0] == 0 && snap->visible_count == 6 && !snap->quit,
              "Circle cleared search query without quitting");
        /* Test IME init failure feedback */
        panel_host_set_ime_script(-1, 0, ORBIS_DIALOG_CANCELED, L"");
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        c->step++;
        break;
    case 8:
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        pad->leftStick.x = 30;
        pad->leftStick.y = 185;
        c->step++;
        break;
    case 9:
        pad->buttons = 0;
        c->step++;
        break;
    case 10:
        CHECK(!snap->ime && strstr(snap->feedback, "keyboard") != NULL, "IME fail feedback='%s'", snap->feedback);
        c->saw_ime_fail = 1;
        /* Open Settings via L2 wheel slot 3 (bottom) */
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        c->step++;
        break;
    case 11:
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        pad->leftStick.x = 128;
        pad->leftStick.y = 235;
        c->step++;
        break;
    case 12:
        pad->buttons = 0;
        c->step++;
        break;
    case 13:
        if (!snap->config_ready)
            return;
        CHECK(snap->page == SETTINGS && snap->in_sidebar == 1, "Settings opened on sidebar");
        /* Enter category 0 */
        pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        c->step++;
        break;
    case 14:
        pad->buttons = 0; /* release CROSS */
        c->step++;
        break;
    case 15:
        pad->buttons = ORBIS_PAD_BUTTON_CROSS; /* toggle row 0 */
        c->sub = 0;
        c->step++;
        break;
    case 16:
        /* Move down to save button with alternating press/release frames */
        if (snap->setting_row < setting_start[1] - setting_start[0]) {
            pad->buttons = (c->sub++ & 1) ? ORBIS_PAD_BUTTON_DOWN : 0;
        } else {
            pad->buttons = ORBIS_PAD_BUTTON_CROSS; /* Save */
            c->step++;
        }
        break;
    case 17:
        pad->buttons = 0; /* release CROSS and wait for SAVE + LOAD */
        if (snap->page == LIBRARY) {
            CHECK(panel_host_server_has_req("POST", "/api/config", NULL), "Settings posted /api/config");
            c->saw_settings_saved = 1;
            /* Open Downloads via L2 wheel slot 2 (bottom-right) */
            pad->buttons = ORBIS_PAD_BUTTON_L2;
            c->step++;
        }
        break;
    case 18:
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        pad->leftStick.x = 225;
        pad->leftStick.y = 185;
        c->step++;
        break;
    case 19:
        pad->buttons = 0;
        c->step++;
        break;
    case 20:
        if (!snap->links_ready)
            return;
        CHECK(snap->page == DOWNLOADS, "Downloads opened");
        /* Script bad URL (non-HTTP) via IME */
        panel_host_set_ime_script(0, 1, ORBIS_DIALOG_OK, L"ftp://bad.example/file.pkg");
        pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        c->sub = 0;
        c->step++;
        break;
    case 21:
        pad->buttons = 0; /* release CROSS */
        if (snap->ime) {
            c->sub = 1;
            return;
        }
        if (!c->sub || !snap->dest_ready)
            return;
        panel_host_advance_clock_ms(250);
        pad->buttons = ORBIS_PAD_BUTTON_CROSS; /* try enqueue bad URL */
        c->step++;
        break;
    case 22:
        CHECK(strstr(snap->feedback, "HTTP") != NULL, "invalid URL rejected: '%s'", snap->feedback);
        c->saw_url_rejected = 1;
        /* Script valid HTTPS URL via Triangle */
        panel_host_set_ime_script(0, 1, ORBIS_DIALOG_OK, L"https://example.test/game.pkg");
        pad->buttons = ORBIS_PAD_BUTTON_TRIANGLE;
        c->sub = 0;
        c->step++;
        break;
    case 23:
        pad->buttons = 0;
        if (snap->ime) {
            c->sub = 1;
            return;
        }
        if (!c->sub || !snap->dest_ready)
            return;
        panel_host_advance_clock_ms(250);
        pad->buttons = ORBIS_PAD_BUTTON_CROSS; /* enqueue valid URL */
        c->step++;
        break;
    case 24:
        pad->buttons = 0;
        if (snap->link_writing)
            return;
        CHECK(panel_host_server_has_req("POST", "/api/links/add?dest=usb0", "https://example.test/game.pkg"),
              "valid URL enqueued to usb0");
        c->saw_url_enqueued = 1;
        /* Open Phone address page via L2 wheel slot 1 (top-right) */
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        c->step++;
        break;
    case 25:
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        pad->leftStick.x = 225;
        pad->leftStick.y = 70;
        c->step++;
        break;
    case 26:
        pad->buttons = 0;
        c->step++;
        break;
    case 27:
        if (snap->page != PHONE || !*snap->local_url)
            return; /* the address comes from the worker */
        CHECK(snap->page == PHONE && strstr(snap->local_url, "192.168.1.42") != NULL,
              "Phone page resolved IP: '%s'", snap->local_url);
        c->saw_phone_ip = 1;
        pad->buttons = ORBIS_PAD_BUTTON_CIRCLE; /* back to LIBRARY */
        c->step++;
        break;
    case 28:
        pad->buttons = 0; /* release CIRCLE */
        c->step++;
        break;
    case 29:
        pad->buttons = ORBIS_PAD_BUTTON_CIRCLE; /* quit */
        c->step++;
        break;
    default:
        *fq = 1;
        break;
    }
}

typedef struct {
    int step;
    int saw_empty_ok;
} empty_ctx_t;

static void scenario_empty_catalog(const panel_host_snapshot_t *snap, OrbisPadData *pad, int *fq, void *vctx) {
    empty_ctx_t *c = vctx;
    if (wait_if_busy(snap))
        return;
    switch (c->step) {
    case 0:
        if (!snap->online)
            return;
        CHECK(snap->visible_count == 0 && snap->selected == 0, "empty catalog loaded cleanly");
        pad->buttons = ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS |
                       ORBIS_PAD_BUTTON_SQUARE | ORBIS_PAD_BUTTON_TRIANGLE;
        c->step++;
        break;
    case 1:
        CHECK(snap->page == LIBRARY && snap->visible_count == 0 && snap->selected == 0,
              "buttons on empty catalog stay safe");
        pad->buttons = ORBIS_PAD_BUTTON_R1;
        c->step++;
        break;
    case 2:
        CHECK(snap->section == SECTION_RUNNING && snap->visible_count == 0, "R1 on empty catalog");
        c->saw_empty_ok = 1;
        pad->buttons = ORBIS_PAD_BUTTON_CIRCLE;
        c->step++;
        break;
    default:
        *fq = 1;
        break;
    }
}

typedef struct {
    int step;
    int saw_refused;
    int saw_500;
    int saw_trunc;
    int saw_recovered;
} neterr_ctx_t;

static void scenario_network_errors(const panel_host_snapshot_t *snap, OrbisPadData *pad, int *fq, void *vctx) {
    neterr_ctx_t *c = vctx;
    if (wait_if_busy(snap))
        return;
    switch (c->step) {
    case 0: /* Started with PANEL_NET_REFUSED -> offline */
        if (snap->active_task)
            return;
        CHECK(snap->online == 0, "connection refused -> offline");
        c->saw_refused = 1;
        /* Switch to HTTP 500 and press Cross to retry */
        panel_host_server_set_net_mode(PANEL_NET_HTTP_500);
        pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        c->step++;
        break;
    case 1:
        pad->buttons = 0; /* release CROSS */
        c->step++;
        break;
    case 2:
        CHECK(snap->online == 0, "HTTP 500 -> still offline");
        c->saw_500 = 1;
        /* Switch to truncated body and retry */
        panel_host_server_set_net_mode(PANEL_NET_TRUNCATED);
        pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        c->step++;
        break;
    case 3:
        pad->buttons = 0; /* release CROSS */
        c->step++;
        break;
    case 4:
        CHECK(snap->online == 0, "truncated response -> still offline");
        c->saw_trunc = 1;
        /* Switch to PANEL_NET_OK and retry -> recovers! */
        panel_host_server_set_net_mode(PANEL_NET_OK);
        pad->buttons = ORBIS_PAD_BUTTON_CROSS;
        c->step++;
        break;
    case 5:
        pad->buttons = 0;
        c->step++;
        break;
    case 6:
        CHECK(snap->online == 1 && snap->visible_count == 6, "recovered online with 6 games (online=%d vis=%d)",
              snap->online, snap->visible_count);
        c->saw_recovered = 1;
        pad->buttons = ORBIS_PAD_BUTTON_CIRCLE;
        c->step++;
        break;
    default:
        *fq = 1;
        break;
    }
}

/* ---------------- 3. Bounded Deterministic Input Fuzzer ---------------- */

typedef struct {
    uint32_t rng;
    int tick;
    int max_ticks;
    int violations;
} fuzz_ctx_t;

static uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return (*s = x);
}

static void scenario_bounded_fuzz(const panel_host_snapshot_t *snap, OrbisPadData *pad, int *fq, void *vctx) {
    fuzz_ctx_t *f = vctx;
    if (wait_if_busy(snap))
        return;

    /* Assert invariants on EVERY tick */
    if (snap->visible_count == 0) {
        if (snap->selected != 0)
            f->violations++;
    } else if (snap->selected < 0 || snap->selected >= snap->visible_count) {
        f->violations++;
    }
    if (snap->section < 0 || snap->section >= SECTIONS)
        f->violations++;
    if (snap->wheel_at < -1 || snap->wheel_at >= WHEEL_ITEMS)
        f->violations++;
    if (snap->category < 0 || snap->category > 2)
        f->violations++;
    if (snap->page < LIBRARY || snap->page > PHONE)
        f->violations++;

    if (++f->tick >= f->max_ticks) {
        *fq = 1;
        return;
    }

    uint32_t r = xorshift32(&f->rng);
    /* Avoid accidental Circle-quit on LIBRARY or L2-Exit so the full fuzz budget runs. */
    static const uint32_t safe_btns[] = {
        0,
        ORBIS_PAD_BUTTON_UP,
        ORBIS_PAD_BUTTON_DOWN,
        ORBIS_PAD_BUTTON_LEFT,
        ORBIS_PAD_BUTTON_RIGHT,
        ORBIS_PAD_BUTTON_L1,
        ORBIS_PAD_BUTTON_R1,
        ORBIS_PAD_BUTTON_TRIANGLE,
        ORBIS_PAD_BUTTON_CROSS,
        ORBIS_PAD_BUTTON_SQUARE,
        ORBIS_PAD_BUTTON_L2,
        ORBIS_PAD_BUTTON_CIRCLE,
    };
    uint32_t b = safe_btns[r % (sizeof(safe_btns) / sizeof(*safe_btns))];
    if (b == ORBIS_PAD_BUTTON_CIRCLE && snap->page == LIBRARY && !snap->wheel && !snap->query[0])
        b = ORBIS_PAD_BUTTON_DOWN;
    pad->connected = (r & 0x3f) != 0; /* occasional pad disconnect */
    pad->buttons = b;
    pad->leftStick.x = (uint8_t)((r >> 8) & 0xff);
    pad->leftStick.y = (uint8_t)((r >> 16) & 0xff);
    pad->rightStick.x = (uint8_t)((r >> 20) & 0xff);
    pad->rightStick.y = (uint8_t)((r >> 24) & 0xff);
    /* Avoid selecting wheel slot 5 (Exit) during fuzz */
    if (snap->wheel && wheel_point((int)pad->leftStick.x - 128, (int)pad->leftStick.y - 128) == 5)
        pad->leftStick.x = 220;
    if (snap->wheel && wheel_point((int)pad->rightStick.x - 128, (int)pad->rightStick.y - 128) == 5)
        pad->rightStick.x = 220;

    /* Periodically toggle IME scripts and transient network errors */
    if ((f->tick % 40) == 15)
        panel_host_set_ime_script(0, 1, (r & 1) ? ORBIS_DIALOG_OK : ORBIS_DIALOG_CANCELED, L"A");
    if ((f->tick % 60) == 30)
        panel_host_server_set_net_mode(PANEL_NET_HTTP_500);
    if ((f->tick % 60) == 36)
        panel_host_server_set_net_mode(PANEL_NET_OK);
}

/* ---------------- 4. Objective Performance Benchmark (main vs fix/panel-app-rescue) ---------------- */

typedef struct {
    int step;
    int sub;
} bench_ctx_t;

static void scenario_perf_benchmark(const panel_host_snapshot_t *snap, OrbisPadData *pad, int *fq, void *vctx) {
    bench_ctx_t *b = vctx;
    if (wait_if_busy(snap))
        return;
    switch (b->step) {
    case 0: /* Wait for 20-game catalog + visible icons cached */
        if (!snap->online || snap->visible_count < 20 || snap->art_cached < 8)
            return;
        /* Reset metrics AFTER initial load/art-fetch so benchmark measures interactive loop! */
        panel_metrics_reset();
        b->step = 1;
        b->sub = 0;
        break;
    case 1: /* 10 DOWN moves */
        if (b->sub < 10) {
            pad->buttons = (b->sub & 1) ? 0 : ORBIS_PAD_BUTTON_DOWN;
            b->sub++;
            if (b->sub < 20)
                return;
        }
        /* Open L2 wheel and sweep all 6 sectors */
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        b->step = 2;
        b->sub = 0;
        break;
    case 2: {
        static const uint8_t sx[6] = {128, 225, 225, 128, 30, 30};
        static const uint8_t sy[6] = {20, 70, 185, 235, 185, 70};
        if (b->sub < 6) {
            pad->buttons = ORBIS_PAD_BUTTON_L2;
            pad->leftStick.x = sx[b->sub];
            pad->leftStick.y = sy[b->sub];
            b->sub++;
            return;
        }
        /* Release on sector 1 (Phone IP -> triggers synchronous sceNetCtlInit/GetInfo/Term in loop!) */
        pad->buttons = ORBIS_PAD_BUTTON_L2;
        pad->leftStick.x = 225;
        pad->leftStick.y = 70;
        b->step = 3;
        break;
    }
    case 3:
        pad->buttons = 0; /* release -> show_address() */
        b->step = 4;
        break;
    case 4:
        pad->buttons = ORBIS_PAD_BUTTON_CIRCLE; /* return to LIBRARY */
        b->step = 5;
        b->sub = 0;
        break;
    case 5: /* Hold Cross for full 700ms (measures redraw churn during hold) */
        if (b->sub < 46) {
            pad->buttons = ORBIS_PAD_BUTTON_CROSS;
            b->sub++;
            return;
        }
        pad->buttons = 0;
        b->step = 6;
        break;
    case 6:
        pad->buttons = ORBIS_PAD_BUTTON_CIRCLE; /* clean exit */
        b->step = 7;
        break;
    default:
        *fq = 1;
        break;
    }
}

int main(void) {
    test_lane_unit();

    build_catalog_20();
    g_png_fixture = panel_host_load_png_fixture("panel-app/Brand.png", &g_png_len);
    CHECK(g_png_fixture != NULL && g_png_len > 100, "loaded real PNG fixture panel-app/Brand.png (%zu B)", g_png_len);

    int port = panel_host_server_start();
    CHECK(port > 0, "mock loopback HTTP server started on port %d", port);

    panel_host_snapshot_t snap;
    panel_metrics_t m;
    panel_lifecycle_t life;

    /* Scenario 1: Navigation, initial jump, L1/R1 sections, L2 wheel (both sticks + no-sel), Detail, art, exit */
    printf("panel scenario: nav, initial jump, L1/R1 sections, L2 wheel, detail, artwork, flip drain\n");
    panel_host_server_reset();
    panel_host_reset_fakes();
    panel_host_server_set_games(k_catalog_mixed);
    panel_host_server_set_icon_png(g_png_fixture, g_png_len);
    panel_host_set_flip_delay(2); /* exercise pending flip backpressure + exit flip-drain */
    nav_ctx_t nav = {0};
    int rc = panel_host_run(scenario_nav_sections_wheel, &nav, &snap, &m, &life);
    CHECK(rc == 0, "nav scenario exit code 0");
    CHECK(nav.saw_jump_fwd && nav.saw_jump_back && nav.saw_detail && nav.saw_release_no_sel && nav.saw_wheel_left &&
              nav.saw_wheel_right && nav.saw_art,
          "all nav/wheel/art milestones reached");
    check_lifecycle(&life, "nav");

    /* Scenario 2: Holds (short, wheel-cancelled, concurrent refresh mid-hold, full remove & add) */
    printf("panel scenario: holds (short, cancelled, concurrent refresh mid-hold, full remove/add)\n");
    panel_host_server_reset();
    panel_host_reset_fakes();
    panel_host_server_set_games(k_catalog_mixed);
    holds_ctx_t holds = {0};
    rc = panel_host_run(scenario_holds_and_concurrent_refresh, &holds, &snap, &m, &life);
    CHECK(rc == 0, "holds scenario exit code 0");
    CHECK(holds.saw_short_no_action && holds.saw_cancel_wheel_no_action && holds.saw_concurrent_drop &&
              holds.saw_full_remove && holds.saw_full_add,
          "all hold milestones reached");
    check_lifecycle(&life, "holds");

    /* Scenario 3: Search/IME (UTF-8, cancel, init fail), Settings save, Downloads URL + dests + enqueue, Phone IP */
    printf("panel scenario: search/IME, settings, downloads/URL, phone IP\n");
    panel_host_server_reset();
    panel_host_reset_fakes();
    panel_host_server_set_games(k_catalog_mixed);
    ime_settings_ctx_t isc = {0};
    rc = panel_host_run(scenario_ime_settings_downloads, &isc, &snap, &m, &life);
    CHECK(rc == 0, "ime/settings/downloads scenario exit code 0");
    CHECK(isc.saw_search_filter && isc.saw_ime_cancel && isc.saw_ime_fail && isc.saw_settings_saved &&
              isc.saw_url_rejected && isc.saw_url_enqueued && isc.saw_phone_ip,
          "all IME/settings/downloads/phone milestones reached");
    CHECK(life.netctl_init_calls > 0, "Phone page called sceNetCtlInit");
    CHECK(m.blocking_netctl_calls == 0, "the phone address is read by the worker, never by the frame loop (%llu)",
          (unsigned long long)m.blocking_netctl_calls);
    if (life.netctl_without_sysmodule > 0)
        printf("  [repro rc.3] sceNetCtlInit called %d time(s) without sceSysmoduleLoadModule(ORBIS_SYSMODULE_NET_CTL)\n",
               life.netctl_without_sysmodule);
    check_lifecycle(&life, "ime_settings");

    /* Scenario 4: Empty catalog */
    printf("panel scenario: empty catalog\n");
    panel_host_server_reset();
    panel_host_reset_fakes();
    panel_host_server_set_games(k_catalog_empty);
    empty_ctx_t emp = {0};
    rc = panel_host_run(scenario_empty_catalog, &emp, &snap, &m, &life);
    CHECK(rc == 0 && emp.saw_empty_ok, "empty catalog clean");
    check_lifecycle(&life, "empty");

    /* Scenario 5: Network errors (refused, 500, truncated) and recovery */
    printf("panel scenario: network errors and recovery\n");
    panel_host_server_reset();
    panel_host_reset_fakes();
    panel_host_server_set_games(k_catalog_mixed);
    panel_host_server_set_net_mode(PANEL_NET_REFUSED);
    neterr_ctx_t ne = {0};
    rc = panel_host_run(scenario_network_errors, &ne, &snap, &m, &life);
    CHECK(rc == 0 && ne.saw_refused && ne.saw_500 && ne.saw_trunc && ne.saw_recovered,
          "all network error & recovery milestones reached");
    check_lifecycle(&life, "neterr");

    /* Scenario 6: Bounded deterministic input fuzzer (200 ticks, fixed seed) */
    printf("panel scenario: bounded deterministic fuzz (200 ticks, seed 0x5eed1234)\n");
    panel_host_server_reset();
    panel_host_reset_fakes();
    panel_host_server_set_games(k_catalog_mixed);
    panel_host_server_set_icon_png(g_png_fixture, g_png_len);
    fuzz_ctx_t fz = {.rng = 0x5eed1234u, .max_ticks = 200};
    rc = panel_host_run(scenario_bounded_fuzz, &fz, &snap, &m, &life);
    CHECK(rc == 0 && fz.violations == 0, "fuzz completed with %d invariant violations", fz.violations);
    check_lifecycle(&life, "fuzz");

    /* Scenario 7: Objective performance metrics benchmark (for main vs fix/panel-app-rescue comparison) */
    printf("panel benchmark: 20-game interactive workload metrics\n");
    panel_host_server_reset();
    panel_host_reset_fakes();
    panel_host_server_set_games(g_catalog_20);
    panel_host_server_set_icon_png(g_png_fixture, g_png_len);
    bench_ctx_t bench = {0};
    rc = panel_host_run(scenario_perf_benchmark, &bench, &snap, &m, &life);
    CHECK(rc == 0, "benchmark exit code 0");
    CHECK(m.redraws > 0 && m.inputs > 0 && m.full_frame_copies > 0 && m.text_rasterizations > 0,
          "metrics instrumentation recorded real work");
    check_lifecycle(&life, "bench");

    double redraws_per_input = m.inputs ? (double)m.redraws / (double)m.inputs : 0.0;
    printf("  PANEL_METRICS_BENCH full_frame_copies=%llu fb_bytes_written=%llu (%.1f MiB) "
           "redraws=%llu inputs=%llu redraws_per_input=%.2f text_rasters=%llu "
           "art_scalings=%llu blocking_loop=%llu (netctl=%llu usleep=%llu ime=%llu) netctl_no_sysmod=%d\n",
           (unsigned long long)m.full_frame_copies, (unsigned long long)m.fb_bytes_written,
           (double)m.fb_bytes_written / (1024.0 * 1024.0), (unsigned long long)m.redraws,
           (unsigned long long)m.inputs, redraws_per_input, (unsigned long long)m.text_rasterizations,
           (unsigned long long)m.art_scalings, (unsigned long long)m.blocking_loop_calls,
           (unsigned long long)m.blocking_netctl_calls, (unsigned long long)m.blocking_usleep_calls,
           (unsigned long long)m.blocking_ime_calls, life.netctl_without_sysmodule);

    panel_host_server_stop();
    free(g_png_fixture);

    printf("panel_lane_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
}
