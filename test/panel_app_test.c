/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host tests for panel-app state machine, concurrency, lifecycle (PANEL-10),
 * bounded redacted logging (PANEL-11), decoupled input/HTTP (PANEL-12),
 * and deterministic multi-seed input fuzzing (PANEL-8/10/12). */
#include "../panel-app/app.h"
#include "../panel-app/applog.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started_count;
    int finished_count;
    int block_requests;
    int release_requests;
    int stop_observed;
    int fail_http;
    int empty_catalog;
    int shuffle_catalog;
    int catalog_version;
    int req_counts[16];
    char last_change_path[256];
    char last_change_form[256];

    /* Mock IME */
    int ime_open_calls;
    int ime_close_calls;
    int ime_active;
    enum ime_status ime_next_status;
    char ime_next_text[256];
} mock_env;

static void mock_init(mock_env *m) {
    memset(m, 0, sizeof(*m));
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->cv, NULL);
    m->ime_next_status = IME_RUNNING;
}

static void mock_destroy(mock_env *m) {
    pthread_cond_destroy(&m->cv);
    pthread_mutex_destroy(&m->mu);
}

static void fill_catalog(library *cat, int version, int empty, int shuffle) {
    memset(cat, 0, sizeof(*cat));
    cat->spanish = 0;
    cat->setup = 1;
    snprintf(cat->version, sizeof(cat->version), "Lodestar 3.0.0-test");
    cat->drive_count = 1;
    snprintf(cat->drives[0].label, sizeof(cat->drives[0].label), "usb0");
    cat->drives[0].free_bytes = 500LL * 1024 * 1024 * 1024;
    if (empty)
        return;

    static const char *const names[] = {
        "Alpha Quest", "Bloodborne", "Celeste", "Dark Souls", "Elden Ring",
        "Final Fantasy", "God of War", "Hollow Knight", "Inside", "Journey",
        "Kena", "Lies of P", "Metaphor", "Nier Automata", "Okami",
        "Persona 5", "Quake", "Returnal", "Sekiro", "Tunic",
    };
    static const enum game_state states[] = {READY, REMOVED, RUNNING, OFFLINE, RESTART};
    int total = (int)(sizeof(names) / sizeof(*names));
    int start = shuffle ? (version % 5) : 0;
    int count = shuffle ? (total - (version % 6)) : total;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % total;
        game *g = &cat->games[cat->count++];
        snprintf(g->id, sizeof(g->id), "CUSA%05d", 10000 + idx);
        snprintf(g->name, sizeof(g->name), "%s", names[idx]);
        g->state = states[(idx + (shuffle ? version : 0)) % 5];
        g->in_place = (idx % 3) == 0;
        snprintf(g->source, sizeof(g->source), "/mnt/usb0/ps4");
    }
}

static panel_result mock_perform(void *ctx, const panel_request *req, const int *stop) {
    mock_env *m = ctx;
    panel_result r = {.kind = req->kind, .http_status = 200, .elapsed_ms = 1};

    pthread_mutex_lock(&m->mu);
    if ((int)req->kind >= 0 && (int)req->kind < 16)
        m->req_counts[req->kind]++;
    if (req->kind == TASK_CHANGE) {
        snprintf(m->last_change_path, sizeof(m->last_change_path), "%s", req->path);
        snprintf(m->last_change_form, sizeof(m->last_change_form), "%s", req->form);
    }
    m->started_count++;
    pthread_cond_broadcast(&m->cv);

    while (m->block_requests && !m->release_requests) {
        if (stop && __atomic_load_n(stop, __ATOMIC_ACQUIRE)) {
            m->stop_observed = 1;
            pthread_mutex_unlock(&m->mu);
            r.ok = 0;
            r.http_status = 0;
            return r;
        }
        pthread_cond_wait(&m->cv, &m->mu);
    }

    int fail = m->fail_http;
    int empty = m->empty_catalog;
    int shuffle = m->shuffle_catalog;
    int ver = ++m->catalog_version;
    pthread_mutex_unlock(&m->mu);

    if (stop && __atomic_load_n(stop, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&m->mu);
        m->stop_observed = 1;
        pthread_mutex_unlock(&m->mu);
        return r;
    }

    if (fail) {
        r.ok = 0;
        r.http_status = 500;
        snprintf(r.message, sizeof(r.message), "Injected HTTP failure");
    } else {
        switch (req->kind) {
        case TASK_LOAD:
            r.catalog = calloc(1, sizeof(*r.catalog));
            assert(r.catalog != NULL);
            fill_catalog(r.catalog, ver, empty, shuffle);
            r.ok = 1;
            break;
        case TASK_STATUS:
            r.ok = (ver % 2) == 0;
            break;
        case TASK_CONFIG:
            r.ok = 1;
            r.config.dlc_mode = 1;
            r.config.language = 0;
            r.config.batch_size = 5;
            r.config.internal = 0;
            break;
        case TASK_LINKS:
            r.ok = 1;
            r.links.configured = 1;
            r.links.count = 1;
            snprintf(r.links.jobs[0].name, sizeof(r.links.jobs[0].name), "Game.pkg");
            snprintf(r.links.jobs[0].state, sizeof(r.links.jobs[0].state), "queued");
            break;
        case TASK_DESTS:
            r.ok = 1;
            r.dests.count = 2;
            snprintf(r.dests.items[0].id, sizeof(r.dests.items[0].id), "usb0");
            snprintf(r.dests.items[0].label, sizeof(r.dests.items[0].label), "USB 0");
            snprintf(r.dests.items[1].id, sizeof(r.dests.items[1].id), "internal");
            snprintf(r.dests.items[1].label, sizeof(r.dests.items[1].label), "Console");
            break;
        case TASK_ART:
            memcpy(r.cover.id, req->id, sizeof(req->id));
            r.cover.pixels = malloc((size_t)ART_SIZE * ART_SIZE * 4);
            r.cover.thumb64 = malloc((size_t)ART_THUMB * ART_THUMB * 4);
            assert(r.cover.pixels && r.cover.thumb64);
            memset(r.cover.pixels, 0x7f, (size_t)ART_SIZE * ART_SIZE * 4);
            memset(r.cover.thumb64, 0x7f, (size_t)ART_THUMB * ART_THUMB * 4);
            r.cover.accent = 0xff336699u;
            r.cover.opaque = 1;
            r.ok = 1;
            break;
        case TASK_ENQUEUE:
        case TASK_SAVE:
        case TASK_SCAN:
        case TASK_CHANGE:
            r.ok = 1;
            snprintf(r.message, sizeof(r.message), "Done");
            break;
        case TASK_ADDR:
            r.ok = 1;
            snprintf(r.local_url, sizeof(r.local_url), "http://192.168.1.50:8780");
            break;
        default:
            break;
        }
    }

    pthread_mutex_lock(&m->mu);
    m->finished_count++;
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mu);
    return r;
}

static int mock_ime_open(void *ctx, enum ime_target target, const char *initial_utf8, size_t max_chars) {
    mock_env *m = ctx;
    (void)target;
    (void)initial_utf8;
    (void)max_chars;
    m->ime_open_calls++;
    m->ime_active = 1;
    m->ime_next_status = IME_RUNNING;
    return 0;
}

static enum ime_status mock_ime_poll(void *ctx, char *out_utf8, size_t out_cap) {
    mock_env *m = ctx;
    if (m->ime_next_status == IME_OK)
        snprintf(out_utf8, out_cap, "%s", m->ime_next_text);
    return m->ime_next_status;
}

static void mock_ime_close(void *ctx) {
    mock_env *m = ctx;
    m->ime_close_calls++;
    m->ime_active = 0;
}

static panel_platform make_platform(mock_env *m) {
    return (panel_platform){
        .ctx = m,
        .perform = mock_perform,
        .ime_open = mock_ime_open,
        .ime_poll = mock_ime_poll,
        .ime_close = mock_ime_close,
    };
}

static void wait_started(mock_env *m, int target) {
    pthread_mutex_lock(&m->mu);
    while (m->started_count < target)
        pthread_cond_wait(&m->cv, &m->mu);
    pthread_mutex_unlock(&m->mu);
}

static void wait_finished(mock_env *m, int target) {
    pthread_mutex_lock(&m->mu);
    while (m->finished_count < target)
        pthread_cond_wait(&m->cv, &m->mu);
    pthread_mutex_unlock(&m->mu);
}

/* Test 1 (PANEL-10): Shutdown cooperatively aborts a blocked worker and joins cleanly. */
static void test_cooperative_shutdown_and_no_post_quit_mutation(void) {
    mock_env m;
    mock_init(&m);
    m.block_requests = 1;

    applog log;
    applog_open(&log, NULL, 4096);

    panel_app app;
    assert(panel_app_init(&app, make_platform(&m), &log, 0) == 0);
    assert(panel_app_start_worker(&app) == 0);

    panel_pad idle = {.connected = 1};
    panel_app_tick(&app, &idle, 100);
    wait_started(&m, 1); /* Worker is now blocked inside TASK_LOAD */

    /* User presses Circle on root to quit while HTTP is still blocked. */
    panel_pad circle = {.connected = 1, .buttons = PANEL_BTN_CIRCLE};
    panel_app_tick(&app, &circle, 116);
    assert(app.quit == 1);

    /* After quit, further ticks must NOT mutate state or schedule new work. */
    enum page page_before = app.page;
    panel_pad down = {.connected = 1, .buttons = PANEL_BTN_DOWN};
    panel_app_tick(&app, &down, 132);
    assert(app.page == page_before && app.quit == 1);

    /* Signal stop, wake blocked mock (simulating poll() noticing stop), and join cleanly. */
    __atomic_store_n(&app.stop_flag, 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&m.mu);
    pthread_cond_broadcast(&m.cv);
    pthread_mutex_unlock(&m.mu);

    panel_app_destroy(&app);
    assert(m.stop_observed == 1);
    assert(!app.worker_running);

    applog_close(&log);
    mock_destroy(&m);
}

/* Test 2: pending_write is never clobbered by pending_read while worker is busy. */
static void test_write_not_clobbered_by_read(void) {
    mock_env m;
    mock_init(&m);
    applog log;
    applog_open(&log, NULL, 4096);

    panel_app app;
    assert(panel_app_init(&app, make_platform(&m), &log, 0) == 0);
    assert(panel_app_start_worker(&app) == 0);

    panel_pad idle = {.connected = 1};
    panel_app_tick(&app, &idle, 100);
    wait_finished(&m, 1); /* Initial TASK_LOAD completes */
    panel_app_tick(&app, &idle, 116);
    assert(app.online == 1 && app.catalog && app.catalog->count == 20);

    /* Block worker on a periodic library refresh (read), then queue BOTH a write and a read. */
    pthread_mutex_lock(&m.mu);
    m.block_requests = 1;
    m.release_requests = 0;
    int before_start = m.started_count;
    pthread_mutex_unlock(&m.mu);

    /* Advance clock past library refresh interval so worker starts TASK_LOAD and blocks. */
    panel_app_tick(&app, &idle, 60000);
    wait_started(&m, before_start + 1);

    /* Now worker is blocked inside TASK_LOAD (a read).
     * 1. Open wheel -> sector 0 (Scan = up: lx=0, ly=-110) -> queues TASK_SCAN into pending_write. */
    panel_pad l2 = {.connected = 1, .buttons = PANEL_BTN_L2};
    panel_pad l2_up = {.connected = 1, .buttons = PANEL_BTN_L2, .lx = 0, .ly = -110};
    panel_app_tick(&app, &l2, 60100);
    panel_app_tick(&app, &l2_up, 60116);
    assert(app.wheel_at == 0);
    panel_app_tick(&app, &idle, 60132); /* queues TASK_SCAN into pending_write */

    /* 2. Open wheel -> sector 1 (Send from phone = up-right: lx=90, ly=-50) -> queues TASK_ADDR into pending_read. */
    panel_pad l2_ur = {.connected = 1, .buttons = PANEL_BTN_L2, .lx = 90, .ly = -50};
    panel_app_tick(&app, &l2, 60200);
    panel_app_tick(&app, &l2_ur, 60216);
    assert(app.wheel_at == 1);
    panel_app_tick(&app, &idle, 60232); /* queues TASK_ADDR into pending_read */

    /* Verify BOTH pending_write AND pending_read exist simultaneously without clobbering each other! */
    assert(app.pending_write.kind == TASK_SCAN);
    assert(app.pending_read.kind == TASK_ADDR);

    /* Unblock worker and verify both execute. */
    pthread_mutex_lock(&m.mu);
    m.release_requests = 1;
    m.block_requests = 0;
    pthread_cond_broadcast(&m.cv);
    pthread_mutex_unlock(&m.mu);

    int target_done = before_start + 3;
    for (int t = 60300; t < 65000; t += 16) {
        panel_app_tick(&app, &idle, t);
        pthread_mutex_lock(&m.mu);
        int done = m.finished_count;
        pthread_mutex_unlock(&m.mu);
        if (done >= target_done)
            break;
        wait_finished(&m, done + 1);
    }
    panel_app_tick(&app, &idle, 65100);
    pthread_mutex_lock(&m.mu);
    assert(m.req_counts[TASK_SCAN] >= 1);
    assert(m.req_counts[TASK_ADDR] >= 1);
    pthread_mutex_unlock(&m.mu);

    panel_app_destroy(&app);
    applog_close(&log);
    mock_destroy(&m);
}

/* Test 3: IME preserves recorded target across page changes and suppresses post-close button bounce. */
static void test_ime_target_and_cooldown(void) {
    mock_env m;
    mock_init(&m);
    applog log;
    applog_open(&log, NULL, 4096);

    panel_app app;
    assert(panel_app_init(&app, make_platform(&m), &log, 0) == 0);
    assert(panel_app_start_worker(&app) == 0);

    panel_pad idle = {.connected = 1};
    panel_app_tick(&app, &idle, 100);
    wait_finished(&m, 1);
    panel_app_tick(&app, &idle, 116);

    /* Open search IME via L2 wheel sector 4 (Search). */
    panel_pad l2 = {.connected = 1, .buttons = PANEL_BTN_L2};
    panel_pad l2_search = {.connected = 1, .buttons = PANEL_BTN_L2, .lx = -90, .ly = 50};
    panel_app_tick(&app, &l2, 200);
    panel_app_tick(&app, &l2_search, 216);
    assert(app.wheel_at == 4);
    panel_app_tick(&app, &idle, 232);
    assert(app.ime == 1 && app.ime_target == IME_SEARCH);
    assert(m.ime_open_calls == 1);

    /* Even if page changes unexpectedly before IME closes, result still goes to query (not entered_url). */
    app.page = DOWNLOADS;
    snprintf(m.ime_next_text, sizeof(m.ime_next_text), "Blood");
    m.ime_next_status = IME_OK;
    panel_app_tick(&app, &idle, 300);
    assert(app.ime == 0);
    assert(!strcmp(app.query, "Blood"));
    assert(app.entered_url[0] == '\0');
    assert(m.ime_close_calls == 1);

    /* Pressing Circle immediately within cooldown (300 + 100ms < 300 + 220ms) must NOT quit! */
    app.page = LIBRARY;
    app.query[0] = '\0';
    panel_pad circle = {.connected = 1, .buttons = PANEL_BTN_CIRCLE};
    panel_app_tick(&app, &circle, 400);
    assert(app.quit == 0);

    /* Release Circle, wait past cooldown (550ms), press Circle -> quits cleanly! */
    panel_app_tick(&app, &idle, 540);
    panel_app_tick(&app, &circle, 560);
    assert(app.quit == 1);

    panel_app_destroy(&app);
    applog_close(&log);
    mock_destroy(&m);
}

/* Test 4 (PANEL-12): L2 wheel stays instantaneous while HTTP is blocked and pauses art fetches. */
static void test_wheel_and_holds_decoupled_from_http(void) {
    mock_env m;
    mock_init(&m);
    applog log;
    applog_open(&log, NULL, 4096);

    panel_app app;
    assert(panel_app_init(&app, make_platform(&m), &log, 0) == 0);
    assert(panel_app_start_worker(&app) == 0);

    panel_pad idle = {.connected = 1};
    panel_app_tick(&app, &idle, 100);
    wait_finished(&m, 1);
    /* Set block_requests BEFORE tick 116 so the first background TASK_ART blocks cleanly. */
    pthread_mutex_lock(&m.mu);
    m.block_requests = 1;
    m.release_requests = 0;
    pthread_mutex_unlock(&m.mu);

    panel_app_tick(&app, &idle, 116);
    assert(app.online == 1);
    wait_started(&m, 2); /* Worker is now blocked inside TASK_ART #1 */

    /* Open L2 wheel and unblock worker while wheel is active:
     * 1. Every stick sweep tick returns instantaneously without blocking on HTTP.
     * 2. No NEW TASK_ART is submitted while the wheel stays open! */
    static const int dirs[6][2] = {
        {0, -100}, {90, -50}, {90, 50}, {0, 100}, {-90, 50}, {-90, -50}
    };
    int64_t t = 500;
    panel_pad l2_open = {.connected = 1, .buttons = PANEL_BTN_L2};
    panel_app_tick(&app, &l2_open, t);
    assert(app.wheel == 1 && app.wheel_at == -1);
    t += 16;

    pthread_mutex_lock(&m.mu);
    int art_before = m.req_counts[TASK_ART];
    m.release_requests = 1;
    m.block_requests = 0;
    pthread_cond_broadcast(&m.cv);
    pthread_mutex_unlock(&m.mu);
    wait_finished(&m, 2); /* TASK_ART #1 completes while wheel is open */

    for (int s = 0; s < 6; s++) {
        panel_pad p = {.connected = 1, .buttons = PANEL_BTN_L2, .lx = dirs[s][0], .ly = dirs[s][1]};
        panel_app_tick(&app, &p, t);
        assert(app.wheel == 1 && app.wheel_at == s);
        t += 16;
    }

    /* Verify zero new TASK_ART requests were submitted while wheel was open. */
    pthread_mutex_lock(&m.mu);
    assert(m.req_counts[TASK_ART] == art_before);
    pthread_mutex_unlock(&m.mu);

    /* Cancel wheel with Circle while L2 held. */
    panel_pad cancel = {.connected = 1, .buttons = PANEL_BTN_L2 | PANEL_BTN_CIRCLE};
    panel_app_tick(&app, &cancel, t);
    assert(app.wheel == 0 && app.quit == 0);

    panel_app_destroy(&app);
    applog_close(&log);
    mock_destroy(&m);
}

/* Test 5 (PANEL-11): Bounded rotated log, in-memory ring, and URL/password redaction. */
static void test_applog_rotation_and_redaction(void) {
    char safe[128];
    applog_safe_path("/api/links?url=https%3A%2F%2Fsecret.host%2Ffile.zip&pass=hunter2&title_id=CUSA00001&dest=usb0",
                     safe, sizeof(safe));
    assert(!strstr(safe, "secret.host"));
    assert(!strstr(safe, "hunter2"));
    assert(strstr(safe, "title_id=CUSA00001") != NULL);
    assert(strstr(safe, "dest=usb0") != NULL);

    applog_safe_path("/api/remove?title_id=CUSA12345&token=abc123", safe, sizeof(safe));
    assert(!strstr(safe, "abc123"));
    assert(strstr(safe, "title_id=CUSA12345") != NULL);

    char path[128], old_path[144];
    snprintf(path, sizeof(path), "/tmp/lodestar_applog_test_%d.log", (int)getpid());
    snprintf(old_path, sizeof(old_path), "%s.old", path);
    unlink(path);
    unlink(old_path);

    applog log;
    applog_open(&log, path, 1024);
    for (int i = 0; i < 80; i++)
        applog_write(&log, "test", "event number=%d payload=0123456789abcdef", i);

    struct stat st = {0}, st_old = {0};
    assert(stat(path, &st) == 0 && st.st_size > 0 && st.st_size <= 1024 + 128);
    assert(stat(old_path, &st_old) == 0 && st_old.st_size > 0);
    char ring_buf[16384];
    size_t ring_len = applog_ring_copy(&log, ring_buf, sizeof(ring_buf));
    assert(ring_len > 0 && strstr(ring_buf, "number=79") != NULL && !strstr(ring_buf, "number=0 "));

    applog_close(&log);
    unlink(path);
    unlink(old_path);

    /* Unwritable path must still work safely via the in-memory ring. */
    applog fallback;
    applog_open(&fallback, "/nonexistent_dir_xyz/sub/panel-app.log", 1024);
    applog_write(&fallback, "init", "ring only test");
    ring_len = applog_ring_copy(&fallback, ring_buf, sizeof(ring_buf));
    assert(ring_len > 0 && strstr(ring_buf, "ring only test") != NULL);
    applog_close(&fallback);
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Test 6: Deterministic multi-seed input fuzzer across pad, wheel, holds, refreshes, IME and errors. */
static void test_deterministic_input_fuzz(void) {
    for (uint32_t seed = 1; seed <= 5; seed++) {
        uint32_t rng = seed * 0x9e3779b9u + 1u;
        mock_env m;
        mock_init(&m);
        m.shuffle_catalog = 1;

        applog log;
        applog_open(&log, NULL, 4096);

        panel_app app;
        assert(panel_app_init(&app, make_platform(&m), &log, (int)(seed & 1)) == 0);
        assert(panel_app_start_worker(&app) == 0);

        int64_t now = 1000;
        panel_pad pad = {.connected = 1};
        int hold_ticks_remaining = 0;
        uint32_t hold_btn = 0;

        for (int step = 0; step < 3000 && !app.quit; step++) {
            uint32_t r = xorshift32(&rng);

            /* Periodically toggle error/empty/shuffle modes on mock worker. */
            if ((step % 97) == 0) {
                pthread_mutex_lock(&m.mu);
                m.fail_http = (r & 7) == 0;
                m.empty_catalog = (r & 15) == 1;
                m.shuffle_catalog = (r & 3) != 0;
                pthread_mutex_unlock(&m.mu);
            }

            /* Resolve open IME deterministically. */
            if (m.ime_active && (r % 5) == 0) {
                m.ime_next_status = (r & 1) ? IME_OK : IME_CANCELED;
                snprintf(m.ime_next_text, sizeof(m.ime_next_text),
                         (r & 2) ? "https://example.invalid/a.pkg" : "a");
            }

            pad.connected = (r % 64) != 0;
            pad.lx = 0;
            pad.ly = 0;
            pad.rx = 0;
            pad.ry = 0;

            if (hold_ticks_remaining > 0) {
                pad.buttons = hold_btn;
                hold_ticks_remaining--;
            } else {
                pad.buttons = 0;
                unsigned action = r % 20;
                switch (action) {
                case 0: pad.buttons = PANEL_BTN_UP; break;
                case 1: pad.buttons = PANEL_BTN_DOWN; break;
                case 2: pad.buttons = PANEL_BTN_LEFT; break;
                case 3: pad.buttons = PANEL_BTN_RIGHT; break;
                case 4: pad.buttons = PANEL_BTN_L1; break;
                case 5: pad.buttons = PANEL_BTN_R1; break;
                case 6: pad.buttons = PANEL_BTN_CROSS; break;
                case 7: pad.buttons = PANEL_BTN_TRIANGLE; break;
                case 8: pad.buttons = PANEL_BTN_SQUARE; break;
                case 9:
                    /* Hold L2 + random stick direction */
                    pad.buttons = PANEL_BTN_L2;
                    pad.lx = (int)(xorshift32(&rng) % 255) - 127;
                    pad.ly = (int)(xorshift32(&rng) % 255) - 127;
                    if ((r & 32) != 0)
                        pad.buttons |= PANEL_BTN_CIRCLE; /* cancel wheel */
                    break;
                case 10:
                    /* Start a multi-tick hold (either incomplete or complete) */
                    hold_btn = (r & 1) ? PANEL_BTN_CROSS : PANEL_BTN_SQUARE;
                    hold_ticks_remaining = (r & 2) ? 55 : 15;
                    pad.buttons = hold_btn;
                    break;
                case 11:
                    /* Stick navigation */
                    pad.ly = (r & 1) ? 110 : -110;
                    break;
                case 12:
                    /* Occasionally press Circle (spare root until end so fuzz runs full 3000 steps) */
                    if (app.page != LIBRARY || app.query[0] || app.wheel || step > 2800)
                        pad.buttons = PANEL_BTN_CIRCLE;
                    break;
                default:
                    break;
                }
            }

            panel_app_tick(&app, &pad, now);

            /* Assert state invariants on EVERY tick */
            assert(app.page >= LIBRARY && app.page <= DOWNLOADS);
            assert(app.wheel_at >= -1 && app.wheel_at <= 5);
            if (app.visible_count == 0)
                assert(app.selected == 0);
            else
                assert(app.selected >= 0 && app.selected < app.visible_count);

            /* Verify build_view always produces consistent bounds */
            panel_view view;
            panel_app_build_view(&app, now, &view);
            assert(view.page == app.page);
            assert(view.wheel_at >= -1 && view.wheel_at <= 5);

            now += 16;
        }

        panel_app_destroy(&app);
        assert(!app.worker_running);
        applog_close(&log);
        mock_destroy(&m);
    }
}

int main(void) {
    test_cooperative_shutdown_and_no_post_quit_mutation();
    test_write_not_clobbered_by_read();
    test_ime_target_and_cooldown();
    test_wheel_and_holds_decoupled_from_http();
    test_applog_rotation_and_redaction();
    test_deterministic_input_fuzz();
    puts("PANEL-10/11/12 app lifecycle, worker safety, applog and 15000-tick fuzz hold");
    return 0;
}
