/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Firmware, boot, first-run, DB readiness, staging and registration decision harness.
 * Models daemon decisions through host-testable functions (fw.h, bootplan.h, scanreq.h,
 * stage.h) without needing the PS4 kernel. Run with: bash test/run.sh
 * With --report, also prints a markdown table of the PS5 releases after the results.
 * test/run.sh compiles it with -DSTRICT_FIRST_RUN_SAFETY: the rc.3 incident (a panel launch
 * with user 0, then a 120 s timeout that registered 20 games) as hard pass/fail assertions
 * on the rules src/main.c runs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootplan.h"
#include "fw.h"
#include "scanreq.h"
#include "stage.h"

static int g_failed, g_checks;

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

/* kernel_get_fw_version() value for major.minor (BCD, low half as on 13.20: 0x13200006). */
static uint32_t raw_fw(unsigned major, unsigned minor, unsigned low) {
    return (uint32_t)(major / 10) << 28 | (uint32_t)(major % 10) << 24 | (uint32_t)(minor / 10) << 20 |
           (uint32_t)(minor % 10) << 16 | (low & 0xffff);
}

static const char *mode_name(int m) {
    return m == REG_ALL ? "AppInstallAll" : m == REG_TITLEDIR ? "AppInstallTitleDir" : "none (no games added)";
}

/* PS5 system software releases (the sweep below covers every version anyway). */
static const char *const k_releases[] = {
    "1.00",  "1.02",  "1.05",  "1.10",  "1.12",  "1.14",  "2.00",  "2.20",  "2.25",  "2.26",  "2.30",  "2.50",
    "2.70",  "3.00",  "3.10",  "3.20",  "3.21",  "4.00",  "4.02",  "4.03",  "4.50",  "4.51",  "5.00",  "5.02",
    "5.10",  "5.50",  "6.00",  "6.02",  "6.50",  "7.00",  "7.01",  "7.20",  "7.40",  "7.60",  "7.61",  "8.00",
    "8.20",  "8.40",  "8.60",  "9.00",  "9.05",  "9.20",  "9.40",  "9.60",  "10.00", "10.01", "10.20", "10.40",
    "10.60", "11.00", "11.20", "11.40", "11.60", "12.00", "12.20", "12.40", "12.60", "13.00", "13.20", NULL};

/* Simulated boot/registration state machine driving src/bootplan.h and src/scanreq.h. */
typedef struct {
    int lock_fd;
    int has_config;
    int has_prior_state;
    int setup_active;
    time_t setup_start;
    int panel_installed;
    int panel_done_marker;
    int panel_opened;
    int panel_db_reg;
    int panel_fetching;
    int open_games;
    int fg_rc;
    int32_t fg_user;
    int db_ready;
    int db_corrupt_marker;
    int reg_mode;
    int user_app_masked;
    int library_games;
    int registered_games;
    int launched_count;
    uint32_t last_launch_user;
    int scans_run;
} boot_sim_t;

static void sim_init(boot_sim_t *s, time_t t0) {
    memset(s, 0, sizeof(*s));
    s->lock_fd = 3;
    s->setup_start = t0;
    s->fg_rc = 0;
    s->fg_user = 0x10000000;
    s->db_ready = 1;
    s->reg_mode = REG_ALL;
}

static void sim_tick(boot_sim_t *s, time_t now, int upload_active, int install_waiting) {
    if (boot_lock_state(s->lock_fd) == BOOT_LOCK_CONFLICT)
        return;

    boot_panel_step_t p =
        boot_panel_app_plan(s->panel_installed, s->panel_done_marker, s->setup_active, s->panel_opened,
                            s->panel_db_reg, s->open_games, s->panel_fetching, (long)(now - s->setup_start), 20,
                            upload_active, install_waiting);
    if (p.mark_done)
        s->panel_done_marker = 1;
    if (p.set_opened)
        s->panel_opened = 1;
    if (p.launch_app) {
        uint32_t u = 0;
        if (boot_launch_user(s->fg_rc, s->fg_user, &u)) {
            s->launched_count++;
            s->last_launch_user = u;
        }
    }
    if (p.start_fetch)
        s->panel_fetching = 1;

    boot_scan_gate_t gate = boot_scan_gate(0, s->setup_active, s->reg_mode);
    if (gate == BOOT_SCAN_RUN && boot_db_usable(s->db_ready, s->db_corrupt_marker)) {
        s->scans_run++;
        s->registered_games = s->library_games;
    }
}

static void test_lock_and_first_start(void) {
    printf("lock port and first start detection\n");
    CHECK(boot_lock_state(5) == BOOT_LOCK_ACQUIRED, "valid lock fd");
    CHECK(boot_lock_state(0) == BOOT_LOCK_ACQUIRED, "fd 0 is still valid");
    CHECK(boot_lock_state(-1) == BOOT_LOCK_CONFLICT, "port 9032 already held (PS4Mount or duplicate Lodestar)");

    CHECK(boot_first_start(0, 0) == 1, "fresh install with no config or catalog/src");
    CHECK(boot_first_start(1, 0) == 0, "saved config.ini is not first start");
    CHECK(boot_first_start(0, 1) == 0, "existing catalog-*.tsv or *.src is not first start");
    CHECK(boot_first_start(1, 1) == 0, "both present is not first start");
}

static void test_foreground_user_and_panel_lifecycle(void) {
    printf("foreground user and panel app launch decisions\n");
    CHECK(boot_fg_user_valid(0, 0x10000000) == 1, "standard PS4 user id");
    CHECK(boot_fg_user_valid(0, 1) == 0, "small ids belong to the system's pseudo users");
    CHECK(boot_fg_user_valid(0, 0x100) == 1, "first id above the pseudo users");
    CHECK(boot_fg_user_valid(0, 0) == 0, "user 0 is invalid (triggers tbl_appbrowse_0000000000)");
    CHECK(boot_fg_user_valid(0, -1) == 0, "negative user id is invalid");
    CHECK(boot_fg_user_valid(-1, 0x10000000) == 0, "failed sceUserServiceGetForegroundUser syscall");
    CHECK(boot_fg_user_valid(-1, 0) == 0, "failed syscall with zero user");

    /* Panel app lifecycle states: missing vs installed, delay, upload/install waiting, open game */
    boot_panel_step_t p = boot_panel_app_plan(0, 0, 1, 0, 0, 0, 0, 5, 20, 0, 0);
    CHECK(!p.mark_done && !p.launch_app && !p.start_fetch, "before PANEL_APP_DELAY: do not fetch");

    p = boot_panel_app_plan(0, 0, 1, 0, 0, 0, 0, 20, 20, 0, 0);
    CHECK(p.start_fetch && !p.launch_app, "at PANEL_APP_DELAY without tile: start fetch");

    p = boot_panel_app_plan(0, 0, 1, 0, 0, 0, 0, 25, 20, 1, 0);
    CHECK(!p.start_fetch, "upload_active defers panel app fetch");

    p = boot_panel_app_plan(0, 0, 1, 0, 0, 0, 0, 25, 20, 0, 1);
    CHECK(!p.start_fetch, "install_waiting defers panel app fetch");

    p = boot_panel_app_plan(0, 1, 1, 0, 0, 0, 0, 30, 20, 0, 0);
    CHECK(!p.start_fetch, "done marker prevents refetch after uninstall");

    p = boot_panel_app_plan(0, 0, 1, 0, 0, 0, 1, 30, 20, 0, 0);
    CHECK(!p.start_fetch, "fetch already in flight");

    p = boot_panel_app_plan(1, 0, 1, 0, 0, 0, 0, 1, 20, 0, 0);
    CHECK(p.mark_done && !p.launch_app && !p.start_fetch, "installed but not yet in app.db: mark done, wait");

    p = boot_panel_app_plan(1, 1, 1, 0, 1, 1, 0, 1, 20, 0, 0);
    CHECK(p.set_opened && !p.launch_app, "an app already open (or unreadable process table): never launch over it");

    p = boot_panel_app_plan(1, 1, 1, 0, 1, 0, 0, 1, 20, 0, 0);
    CHECK(p.set_opened && p.launch_app, "installed + registered + setup_active + no open game: launch");

    p = boot_panel_app_plan(1, 1, 1, 1, 1, 0, 0, 1, 20, 0, 0);
    CHECK(!p.launch_app, "already opened once: do not relaunch");

    p = boot_panel_app_plan(1, 1, 0, 0, 1, 0, 0, 1, 20, 0, 0);
    CHECK(!p.launch_app, "not first start: do not auto-open");

    /* Valid user launches cleanly; user 0, a failed query and the pseudo users never do */
    uint32_t lu = 0;
    CHECK(boot_launch_user(0, 0x10000000, &lu) == 1 && lu == 0x10000000u, "valid foreground user");
    CHECK(boot_launch_user(0, 0x1B421304, &lu) == 1 && lu == 0x1B421304u, "real local PS4 user id");
    lu = 0xdeadbeefu;
#ifdef STRICT_FIRST_RUN_SAFETY
    CHECK(boot_launch_user(0, 0, &lu) == 0, "STRICT: must not launch LODS00001 with user 0");
    CHECK(boot_launch_user(-1, 0, &lu) == 0, "STRICT: must not launch when GetForegroundUser fails");
    CHECK(boot_launch_user(-1, 0x10000000, &lu) == 0, "STRICT: must not trust user output on error rc");
    CHECK(boot_launch_user(0, -1, &lu) == 0, "STRICT: must not launch with user -1");
    CHECK(boot_launch_user(0, 0xFE, &lu) == 0, "STRICT: pseudo user EVERYONE");
    CHECK(boot_launch_user(0, 0xFF, &lu) == 0, "STRICT: pseudo user SYSTEM");
    CHECK(lu == 0xdeadbeefu, "STRICT: a refused user is never written out");
#endif
}

static void test_library_sizes(void) {
    printf("registration batches for pre-existing libraries (0/1/20/768)\n");
    const int sizes[] = {0, 1, 20, 768};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int n = sizes[i];
        int sz = 0, groups = 0;

        boot_batch_plan(n, 0, 50, &sz, &groups);
        if (n == 0)
            CHECK(sz == 0 && groups == 0, "n=0 unbatched");
        else
            CHECK(sz == n && groups == 1, "n=%d unbatched => 1 group of %d", n, n);

        boot_batch_plan(n, 1, 50, &sz, &groups);
        if (n == 0)
            CHECK(sz == 0 && groups == 0, "n=0 batched");
        else
            CHECK(sz == 50 && groups == (n + 49) / 50, "n=%d batched(50) => %d groups", n, groups);
    }
}

static void test_db_states_and_install_outcomes(void) {
    printf("app.db states, corrupt marker, staging /user/app and AppInstallAll outcomes\n");
    CHECK(boot_db_usable(0, 0) == 0, "unreadable app.db (missing file or no tbl_appinfo, db_ready) blocks scan");
    CHECK(boot_db_usable(0, 1) == 0, "unreadable and flagged");

    CHECK(boot_db_usable(1, 0) == 1, "healthy app.db allows scan");
#ifdef STRICT_FIRST_RUN_SAFETY
    CHECK(boot_db_usable(1, 1) == 0, "STRICT: app.db-corrupt marker must block scan_all");
#endif

    /* Scan gate combinations */
    CHECK(boot_scan_gate(1, 0, REG_ALL) == BOOT_SCAN_SKIP_DELETING, "deleting defers scan");
    CHECK(boot_scan_gate(0, 1, REG_ALL) == BOOT_SCAN_WAIT_SETUP, "setup_active waits");
    CHECK(boot_scan_gate(0, 0, REG_NONE) == BOOT_SCAN_UNSUPPORTED_FW, "REG_NONE skips scan");
    CHECK(boot_scan_gate(0, 0, REG_ALL) == BOOT_SCAN_RUN, "normal REG_ALL runs");
    CHECK(boot_scan_gate(0, 0, REG_TITLEDIR) == BOOT_SCAN_RUN, "normal REG_TITLEDIR runs");

    /* Staging /user/app masking and incomplete unmount */
    CHECK(boot_stage_mask_allowed(REG_ALL, 0, 1) == 1, "REG_ALL with no open games masks /user/app");
    CHECK(boot_stage_mask_allowed(REG_ALL, 0, 20) == 1, "REG_ALL batch of 20 masks /user/app");
    CHECK(boot_stage_mask_allowed(REG_ALL, 1, 20) == 0, "open game prevents /user/app mask");
    CHECK(boot_stage_mask_allowed(REG_TITLEDIR, 0, 20) == 0, "REG_TITLEDIR never masks /user/app");
    CHECK(boot_stage_mask_allowed(REG_NONE, 0, 20) == 0, "REG_NONE never masks /user/app");
    CHECK(boot_stage_mask_allowed(REG_ALL, 0, 0) == 0, "0 staged titles does not mask");

    CHECK(boot_stage_unmask_ok(0) == 1, "clean unmask of /user/app");
    CHECK(boot_stage_unmask_ok(1) == 0, "incomplete unmount leaves /user/app masked");

    /* AppInstallAll outcomes */
    CHECK(boot_install_all_outcome(REG_ALL, 0, 0, 20, 20, 0) == BOOT_REG_OK, "all 20 registered");
    CHECK(boot_install_all_outcome(REG_ALL, 0, 0, 12, 20, 0) == BOOT_REG_PARTIAL, "12/20 partial registration");
    CHECK(boot_install_all_outcome(REG_ALL, 0, 0, 0, 20, 0) == BOOT_REG_PARTIAL, "rc=0 but 0/20 registered");
    CHECK(boot_install_all_outcome(REG_ALL, 0, -1, 0, 20, 0) == BOOT_REG_FAILED, "AppInstallAll error rc");
    CHECK(boot_install_all_outcome(REG_ALL, 1, 0, 5, 20, 0) == BOOT_REG_BUSY_TIMEOUT, "AppInstallAll busy timeout");
    CHECK(boot_install_all_outcome(REG_ALL, 0, 0, 20, 20, 1) == BOOT_REG_STUCK_MASKED, "incomplete /user/app unmount");
    CHECK(boot_install_all_outcome(REG_NONE, 0, 0, 0, 20, 0) == BOOT_REG_UNSUPPORTED, "unsupported firmware");

    /* Stage cleanup safety (src/stage.h contract): never wipe staging on failed/partial install */
    CHECK(stage_installed_ok(1, 1, 1, 1, 4096, 4096) == 1, "valid + unmounted + confirmed + exact size => safe");
    CHECK(stage_installed_ok(1, 1, 0, 1, 4096, 4096) == 0, "not confirmed registered => keep");
    CHECK(stage_installed_ok(1, 0, 1, 1, 4096, 4096) == 0, "still mounted => keep");
    CHECK(stage_installed_ok(1, 1, 1, 1, 2048, 4096) == 0, "size mismatch => keep");
}

static void test_scan_repetitions(void) {
    printf("scan repetitions and coalescing (src/scanreq.h)\n");
    scanreq_t q;
    scanreq_init(&q);
    time_t t = 1000;
    CHECK(scanreq_due(&q, t) == 1, "initial scan due immediately");
    int manual = 0, hard = 0;
    scan_trigger_t trig = SCAN_TRIG_ROUTINE;
    unsigned s1 = scanreq_begin_why(&q, &manual, &hard, &trig);
    CHECK(s1 == 0 && !manual && !hard && trig == SCAN_TRIG_STARTUP, "first startup scan");

    /* Repeated requests while scan is in progress coalesce into a single immediate follow-up */
    scanreq_request_why(&q, 0, SCAN_TRIG_UPLOAD);
    scanreq_request_why(&q, 1, SCAN_TRIG_MANUAL);
    scanreq_request_hard(&q);
    scanreq_request_why(&q, 0, SCAN_TRIG_CONFIG);
    scanreq_finish(&q, s1, t + 2, scanreq_period(0));

    CHECK(scanreq_due(&q, t + 2) == 1, "stale seq finish must not suppress queued request");
    unsigned s2 = scanreq_begin_why(&q, &manual, &hard, &trig);
    CHECK(s2 > s1 && manual == 1 && hard == 1 && trig == SCAN_TRIG_HARD,
          "coalesced follow-up preserves manual=1, hard=1, trig=HARD");
    scanreq_finish(&q, s2, t + 5, scanreq_period(0));

    /* Next routine scan waits idle period (10s) or gameplay period (120s) */
    CHECK(scanreq_due(&q, t + 5) == 0, "no immediate repeat after clean scan");
    CHECK(scanreq_due(&q, t + 5 + SCANREQ_IDLE_SECONDS - 1) == 0, "still waiting before idle period");
    CHECK(scanreq_due(&q, t + 5 + SCANREQ_IDLE_SECONDS) == 1, "routine scan due after SCANREQ_IDLE_SECONDS");

    /* Failed DB scan keeps manual/hard intent for next epoch without spinning immediately */
    unsigned s3 = scanreq_begin_why(&q, &manual, &hard, &trig);
    scanreq_keep_request_why(&q, 1, 1, SCAN_TRIG_HARD);
    scanreq_finish(&q, s3, t + 100, scanreq_period(1));
    CHECK(scanreq_due(&q, t + 100) == 0, "failed DB scan does not busy-spin in same second");
    CHECK(scanreq_due(&q, t + 100 + SCANREQ_GAMEPLAY_SECONDS) == 1, "retries after gameplay period");
    (void)scanreq_begin_why(&q, &manual, &hard, &trig);
    CHECK(manual == 1 && hard == 1 && trig == SCAN_TRIG_HARD, "kept manual/hard intent survived DB wait");
}

static void test_rc3_timeline(void) {
    printf("rc.3 failure timeline simulation\n");
    boot_sim_t s;
    sim_init(&s, 1000);
    s.has_config = 0;
    s.has_prior_state = 0;
    s.setup_active = boot_first_start(s.has_config, s.has_prior_state);
    s.panel_installed = 1;
    s.panel_db_reg = 1;
    s.fg_rc = 0;
    s.fg_user = 0; /* ShellCore not yet reporting foreground user */
    s.library_games = 20;

    CHECK(s.setup_active == 1, "fresh rc.3 boot enters setup_active");

    /* T+1s: panel_app_step tries to auto-open LODS00001 */
    sim_tick(&s, 1001, 0, 0);
#ifdef STRICT_FIRST_RUN_SAFETY
    CHECK(s.launched_count == 0, "STRICT: LODS00001 must not be launched while foreground user is 0");
#endif

    /* Had it launched with user 0, ShellCore would log DbCorruptionOccured and leave
     * app.db-corrupt; the panel app would crash and nobody would save the settings. */
    if (s.launched_count > 0 && s.last_launch_user == 0u)
        s.db_corrupt_marker = 1;

    /* T+2s..T+10 min: nobody saves the settings. Neither the 120 s of rc.3 nor any later
     * time adopts or registers the library. */
    for (time_t t = 1002; t < 1600; t += 10)
        sim_tick(&s, t, 0, 0);
#ifdef STRICT_FIRST_RUN_SAFETY
    CHECK(s.setup_active == 1, "STRICT: the first start waits for an explicit save");
    CHECK(s.registered_games == 0 && s.scans_run == 0, "STRICT: rc.3 timeout must not register the 20 games");
#endif

    /* The settings are saved: the scan runs, unless the system flagged app.db */
    s.setup_active = 0;
    s.db_corrupt_marker = 1;
    sim_tick(&s, 1601, 0, 0);
#ifdef STRICT_FIRST_RUN_SAFETY
    CHECK(s.scans_run == 0, "STRICT: scan must not run while app.db-corrupt is present");
#endif
    s.db_corrupt_marker = 0;
    sim_tick(&s, 1602, 0, 0);
    CHECK(s.scans_run == 1 && s.registered_games == 20, "after the save and without the marker the scan runs");
}

int main(int argc, char **argv) {
    int report = argc > 1 && !strcmp(argv[1], "--report");
    printf("firmware sweep 0.00 to 99.99\n");
    for (unsigned major = 0; major <= 99; major++)
        for (unsigned minor = 0; minor <= 99; minor++)
            for (int td = 0; td <= 1; td++) {
                uint32_t raw = raw_fw(major, minor, major * 7 + minor); /* any low half */
                unsigned ma, mi;
                char text[16], want[16];
                fw_decode(raw, &ma, &mi);
                CHECK(ma == major && mi == minor, "0x%08x decoded as %u.%u", raw, ma, mi);
                fw_text(raw, text, sizeof(text));
                snprintf(want, sizeof(want), "%u.%02u", major, minor);
                CHECK(!strcmp(text, want), "0x%08x shown as %s", raw, text);
                int m = fw_reg_mode(ma, td);
                if (major >= MIN_FW_INSTALL_ALL)
                    CHECK(m == REG_ALL, "%s with TitleDir %d: %s", text, td, mode_name(m));
                else
                    /* AppInstallAll corrupts the database below 6.00 (issues #11, #13): never used there */
                    CHECK(m == (td ? REG_TITLEDIR : REG_NONE), "%s with TitleDir %d: %s", text, td, mode_name(m));
                CHECK(major >= MIN_FW_INSTALL_ALL || m != REG_ALL, "AppInstallAll chosen on %s", text);
            }

    printf("values seen on consoles\n");
    char text[16];
    fw_text(0x13200006, text, sizeof(text));
    CHECK(!strcmp(text, "13.20"), "%s", text);
    fw_text(0x04500005, text, sizeof(text));
    CHECK(!strcmp(text, "4.50"), "%s", text);
    fw_text(0x05500000, text, sizeof(text));
    CHECK(!strcmp(text, "5.50"), "%s", text);
    CHECK(fw_reg_mode(13, 0) == REG_ALL, "13.20 without TitleDir");  /* PS4Mount test console */
    CHECK(fw_reg_mode(5, 1) == REG_TITLEDIR, "5.50 with TitleDir");   /* issue #11 */
    CHECK(fw_reg_mode(4, 1) == REG_TITLEDIR, "4.50 with TitleDir");   /* issue #13 */
    CHECK(fw_reg_mode(5, 0) == REG_NONE, "5.50 without TitleDir");

    test_lock_and_first_start();
    test_foreground_user_and_panel_lifecycle();
    test_library_sizes();
    test_db_states_and_install_outcomes();
    test_scan_repetitions();
    test_rc3_timeline();

    FILE *f = report ? stdout : NULL;
    if (f)
        fprintf(f, "### Registration method per firmware\n\n| Firmware | With AppInstallTitleDir | Without it |\n|---|---|---|\n");
    for (int i = 0; k_releases[i]; i++) {
        unsigned major = (unsigned)atoi(k_releases[i]), minor = (unsigned)atoi(strchr(k_releases[i], '.') + 1);
        unsigned ma, mi;
        fw_decode(raw_fw(major, minor, 0), &ma, &mi);
        int with = fw_reg_mode(ma, 1), without = fw_reg_mode(ma, 0);
        CHECK(ma == major && mi == minor, "%s", k_releases[i]);
        if (f)
            fprintf(f, "| %s | %s | %s |\n", k_releases[i], mode_name(with), mode_name(without));
    }

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
}
