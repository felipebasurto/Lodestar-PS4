/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Regression & guardrail tests for REG-7, REG-8, REG-9, REG-10:
 *   - app.db-corrupt marker present before startup and appearing mid-batch (preserved untouched)
 *   - klog DbCorruptionOccured during registration (aborts immediately, enqueues no more)
 *   - First start with 20 pre-existing games after the explicit save (1-title pilot + validation +
 *     explicit continuation)
 *   - Staging/unmount failure and DB error during AppInstallAll (prioritizes restoring /user/app,
 *     keeps masked=1 and stage intact if stuck, aborts batch)
 *   - Operational inventory persistence under /data/lodestar (atomic write, round-trip, refuses priv/mms)
 */
#include "dbguard.h"
#include "inventory.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass, g_fail;

#define CHECK(cond, msg)                                                           \
    do {                                                                           \
        if (cond) {                                                                \
            g_pass++;                                                              \
        } else {                                                                   \
            g_fail++;                                                              \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));      \
        }                                                                          \
    } while (0)

static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n && fclose(f) == 0;
    return ok ? 0 : -1;
}

/* REG-7: app.db-corrupt marker present before startup and appearing mid-batch. */
static void test_corrupt_marker_startup_and_midbatch(void) {
    char dir1[] = "/tmp/lodestar-dbguard-1-XXXXXX";
    CHECK(mkdtemp(dir1) != NULL, "mkdtemp dir1");
    char marker1[256];
    snprintf(marker1, sizeof(marker1), "%s/app.db-corrupt", dir1);
    CHECK(write_text(marker1, "evidence-before-start\n") == 0, "seed app.db-corrupt before start");
    struct stat st_before, st_after;
    CHECK(stat(marker1, &st_before) == 0, "stat marker before");

    dbguard_t g1;
    dbguard_init(&g1, dir1);
    reg_breaker_t b1;
    breaker_init(&b1, 0, 0);

    CHECK(dbguard_check_markers(&g1) == 1, "startup detects pre-existing app.db-corrupt");
    CHECK(dbguard_is_corrupt(&g1) == 1, "corrupt state latched");
    CHECK(dbguard_take_warn(&g1) == 1, "warning fires once on latch");
    CHECK(dbguard_take_warn(&g1) == 0, "warning does not spam repeatedly");
    CHECK(breaker_batch_limit(&b1, 20, 5, dbguard_is_corrupt(&g1)) == 0,
          "0 titles registered when app.db-corrupt exists at startup");

    char reason[256];
    CHECK(dbguard_snapshot(&g1, reason, sizeof(reason)) == 1, "snapshot reports corrupt");
    CHECK(strstr(reason, "app.db-corrupt") != NULL, "reason identifies app.db-corrupt");

    /* Evidence preservation: marker must still exist and remain untouched. */
    CHECK(stat(marker1, &st_after) == 0, "app.db-corrupt must NEVER be deleted");
    CHECK(st_after.st_size == st_before.st_size, "app.db-corrupt must NEVER be modified");

    dbguard_destroy(&g1);
    unlink(marker1);
    rmdir(dir1);

    /* Case 2: clean at startup, app.db-corrupt appears mid-batch (after batch 1 of 4). */
    char dir2[] = "/tmp/lodestar-dbguard-2-XXXXXX";
    CHECK(mkdtemp(dir2) != NULL, "mkdtemp dir2");
    char marker2[256];
    snprintf(marker2, sizeof(marker2), "%s/app.db-corrupt", dir2);

    dbguard_t g2;
    dbguard_init(&g2, dir2);
    reg_breaker_t b2;
    breaker_init(&b2, 0, 0);

    int remaining = 20;
    int batches_run = 0;
    int titles_queued = 0;
    while (remaining > 0) {
        int lim = breaker_batch_limit(&b2, remaining, 5, dbguard_check_markers(&g2));
        if (lim <= 0) break;
        batches_run++;
        titles_queued += lim;
        remaining -= lim;
        if (batches_run == 1) {
            /* Mid-batch: ShellCore creates app.db-corrupt. */
            CHECK(write_text(marker2, "mid-batch\n") == 0, "create app.db-corrupt mid-batch");
            CHECK(dbguard_check_markers(&g2) == 1, "mid-batch check latches app.db-corrupt");
            CHECK(dbguard_install_poll(dbguard_is_corrupt(&g2), 1, 0, 1, lim, 1) == INST_POLL_ABORT_DB_ERROR,
                  "active install_all poll aborts immediately when marker appears");
        }
    }
    CHECK(batches_run == 1, "no further batches run after mid-batch app.db-corrupt");
    CHECK(titles_queued == 5, "remaining 15 titles are not queued");
    CHECK(stat(marker2, &st_after) == 0, "mid-batch app.db-corrupt preserved on disk");

    dbguard_destroy(&g2);
    unlink(marker2);
    rmdir(dir2);
}

/* REG-7: klog DbCorruptionOccured during registration aborts and enqueues no more. */
static void test_klog_corruption_during_registration(void) {
    char dir[] = "/tmp/lodestar-dbguard-klog-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp klog");

    dbguard_t g;
    dbguard_init(&g, dir);
    reg_breaker_t b;
    breaker_init(&b, 0, 0);

    /* Benign klog lines must not false-positive. */
    CHECK(dbguard_observe_klog(&g, "[SceShellCore] launchApp(CUSA00001)") == 0, "benign launchApp ignored");
    CHECK(dbguard_observe_klog(&g, "[PlayGoService][End] end (CUSA00001)") == 0, "benign PlayGo End ignored");
    CHECK(dbguard_is_corrupt(&g) == 0, "still clean after benign lines");

    /* Simulated registration loop receives DbCorruptionOccured from real FW 14.00 incident. */
    int enqueued = 0;
    for (int batch = 0; batch < 4; batch++) {
        int lim = breaker_batch_limit(&b, 20 - enqueued, 5, dbguard_is_corrupt(&g));
        if (lim <= 0) break;
        enqueued += lim;
        if (batch == 0) {
            int hit = dbguard_observe_klog(
                &g, "[SceShellCore] DbCorruptionOccured /system_data/priv/mms/app.db detail: no such table: tbl_appbrowse_0000000000");
            CHECK(hit == 1, "DbCorruptionOccured klog line detected");
            CHECK(dbguard_install_poll(dbguard_is_corrupt(&g), 1, 0, 0, lim, 0) == INST_POLL_ABORT_DB_ERROR,
                  "in-flight registration aborts on klog DbCorruptionOccured");
        }
    }
    CHECK(enqueued == 5, "batches 2..4 never enqueued after klog DbCorruptionOccured");
    CHECK(dbguard_is_corrupt(&g) == 1, "corruption stays latched");

    /* Also check all other real error signatures from the FW 14.00 incident. */
    CHECK(dbguard_klog_corrupt("DbCorruptionOccured 0x80f50061") == 1, "0x80f50061 klog matched");
    CHECK(dbguard_klog_corrupt("DbCorruptionOccured 0x809C1147") == 1, "0x809C1147 klog matched");
    CHECK(dbguard_klog_corrupt("DbCorruptionOccured 0x80f50074") == 1, "0x80f50074 klog matched");
    CHECK(dbguard_rc_is_db_error((int)0x80f50061u) == 1, "0x80f50061 rc classified as DB error");
    CHECK(dbguard_rc_is_db_error((int)0x809c1147u) == 1, "0x809c1147 rc classified as DB error");
    CHECK(dbguard_rc_is_db_error((int)0x80f50074u) == 1, "0x80f50074 rc classified as DB error");
    CHECK(dbguard_rc_is_db_error(0) == 0, "0 rc is not a DB error");

    dbguard_destroy(&g);
    rmdir(dir);
}

/* REG-8: after the explicit save, a first start with 20 pre-existing games registers 1 pilot
 * title and holds the rest for explicit continuation. */
static void test_first_start_pilot_breaker(void) {
    reg_breaker_t b;
    breaker_init(&b, 1, 0);
    int pending = 20;
    int lim1 = breaker_batch_limit(&b, pending, 5, 0);
    CHECK(lim1 == 1, "first-start pilot caps 20 pending games to at most 1 title");
    breaker_note_pilot(&b, "CUSA00001");

    /* Pilot succeeds and validates cleanly -> remaining 19 held for explicit continuation. */
    int res = breaker_after_pilot(&b, 0, 1, 1, pending - 1);
    CHECK(res == 1, "pilot validation holds remaining 19 titles");
    CHECK(b.mode == BREAKER_PILOT_HOLD, "breaker enters BREAKER_PILOT_HOLD");
    CHECK(breaker_batch_limit(&b, 19, 5, 0) == 0, "unattended follow-up scan registers 0 of remaining 19");

    /* Explicit operator scan/add resumes bounded batching when DB is healthy. */
    CHECK(breaker_resume(&b, 1) == 0, "resume refused if db_corrupt is active");
    CHECK(breaker_resume(&b, 0) == 1, "explicit resume succeeds when DB is healthy");
    CHECK(breaker_batch_limit(&b, 19, 5, 0) == 5, "normal batch cap (5) applies after explicit resume");

    /* 3. Post-migration pilot failure trips breaker permanently for this run. */
    reg_breaker_t bm;
    breaker_init(&bm, 0, 1);
    CHECK(breaker_batch_limit(&bm, 20, 5, 0) == 1, "post-migration caps to 1 pilot title");
    int fail_res = breaker_after_pilot(&bm, 0, 0, 1, 19);
    CHECK(fail_res == -1, "unconfirmed pilot trips breaker");
    CHECK(bm.mode == BREAKER_TRIPPED, "breaker is BREAKER_TRIPPED");
    CHECK(breaker_batch_limit(&bm, 19, 5, 0) == 0, "tripped breaker blocks further registration");
    CHECK(breaker_resume(&bm, 0) == 0, "tripped breaker cannot be casually resumed");
}

/* REG-9: staging/unmount failure and DB error prioritize restoring /user/app and abort. */
typedef struct {
    int masked;
    int child_mounted[4];
    int gentle_fails_remaining;
    int force_fails;
    int child_unmount_calls;
    int root_gentle_calls;
    int root_force_calls;
    int stage_cleared;
    int child_before_root_ok;
} mock_stage_t;

static int mock_is_masked(void *ctx) {
    return ((mock_stage_t *)ctx)->masked;
}

static int mock_child_mounted(void *ctx, const char *tid) {
    mock_stage_t *m = (mock_stage_t *)ctx;
    int idx = tid[8] - '1';
    return (idx >= 0 && idx < 4) ? m->child_mounted[idx] : 0;
}

static int mock_unmount(void *ctx, const char *path, int force) {
    mock_stage_t *m = (mock_stage_t *)ctx;
    if (strcmp(path, "/user/app") == 0) {
        for (int i = 0; i < 4; i++)
            if (m->child_mounted[i]) m->child_before_root_ok = 0;
        if (force) {
            m->root_force_calls++;
            if (m->force_fails) return -1;
            m->masked = 0;
            return 0;
        }
        m->root_gentle_calls++;
        if (m->gentle_fails_remaining > 0) {
            m->gentle_fails_remaining--;
            return -1;
        }
        m->masked = 0;
        return 0;
    }
    m->child_unmount_calls++;
    size_t len = strlen(path);
    int idx = len > 0 ? path[len - 1] - '1' : -1;
    if (idx >= 0 && idx < 4) {
        if (!force && idx == 1) return -1; /* force fallback on second child */
        m->child_mounted[idx] = 0;
    }
    return 0;
}

static void mock_sleep(void *ctx, int ms) {
    (void)ctx;
    (void)ms;
}

static void mock_stage_clear(void *ctx) {
    ((mock_stage_t *)ctx)->stage_cleared++;
}

static void test_staging_unmount_and_db_error_abort(void) {
    char tids[2][10] = {"CUSA00001", "CUSA00002"};

    /* Case 1: DB error occurs during AppInstallAll; gentle unmount fails 12 times, MNT_FORCE restores /user/app. */
    CHECK(dbguard_install_poll(0, 0, (int)0x80f50061u, 0, 2, 0) == INST_POLL_ABORT_DB_ERROR,
          "0x80f50061 from AppInstallAll triggers immediate DB-error abort");
    CHECK(dbguard_install_poll(0, 0, (int)0x809c1147u, 0, 2, 0) == INST_POLL_ABORT_DB_ERROR,
          "0x809c1147 from AppInstallAll triggers immediate DB-error abort");
    CHECK(dbguard_install_poll(0, 0, (int)0x80f50074u, 0, 2, 0) == INST_POLL_ABORT_DB_ERROR,
          "0x80f50074 from AppInstallAll triggers immediate DB-error abort");

    mock_stage_t m1;
    memset(&m1, 0, sizeof(m1));
    m1.masked = 1;
    m1.child_mounted[0] = 1;
    m1.child_mounted[1] = 1;
    m1.gentle_fails_remaining = 99; /* forces MNT_FORCE fallback */
    m1.force_fails = 0;
    m1.child_before_root_ok = 1;

    stage_unmask_ops_t ops1 = {&m1, mock_is_masked, mock_child_mounted, mock_unmount, mock_sleep, mock_stage_clear};
    int masked_flag1 = 1;
    int rc1 = dbguard_unmask_user_app(&ops1, tids, 2, &masked_flag1);
    CHECK(rc1 == 0, "MNT_FORCE fallback restores /user/app");
    CHECK(m1.child_before_root_ok == 1, "staged children unmounted before /user/app");
    CHECK(m1.root_gentle_calls == 12, "tried gentle unmount retries before force");
    CHECK(m1.root_force_calls == 1, "escalated to MNT_FORCE to restore /user/app");
    CHECK(masked_flag1 == 0, "masked_flag cleared once /user/app proven unmasked");
    CHECK(m1.stage_cleared == 1, "stage cleared after /user/app restored");

    /* Case 2: Total unmount failure (both gentle and force fail) -> keep masked=1, do NOT clear stage, return -1. */
    mock_stage_t m2;
    memset(&m2, 0, sizeof(m2));
    m2.masked = 1;
    m2.child_mounted[0] = 1;
    m2.gentle_fails_remaining = 99;
    m2.force_fails = 1;
    m2.child_before_root_ok = 1;

    stage_unmask_ops_t ops2 = {&m2, mock_is_masked, mock_child_mounted, mock_unmount, mock_sleep, mock_stage_clear};
    int masked_flag2 = 1;
    int rc2 = dbguard_unmask_user_app(&ops2, tids, 1, &masked_flag2);
    CHECK(rc2 == -1, "unmask reports error when /user/app stays masked");
    CHECK(masked_flag2 == 1, "masked_flag stays 1 when /user/app is still masked");
    CHECK(m2.stage_cleared == 0, "stage is NOT cleared while /user/app is still masked");
}

/* REG-10: operational inventory persistence under /data/lodestar (never priv/mms). */
static void test_operational_inventory(void) {
    char dir[] = "/tmp/lodestar-inv-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp inv");
    char path[256], tmp_path[272];
    snprintf(path, sizeof(path), "%s/inventory.tsv", dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/inventory.tsv.tmp", dir);

    inv_list_t *inv = inv_alloc();
    CHECK(inv != NULL, "inv_alloc succeeds");
    CHECK(inv_load(inv, path) == 0, "missing inventory loads as empty");

    inv_entry_t e1;
    memset(&e1, 0, sizeof(e1));
    snprintf(e1.title_id, sizeof(e1.title_id), "CUSA00001");
    snprintf(e1.source, sizeof(e1.source), "/mnt/usb0/ps4/CUSA00001/app.pkg");
    snprintf(e1.mount_mode, sizeof(e1.mount_mode), "hold-organized");
    snprintf(e1.game_ver, sizeof(e1.game_ver), "01.00");
    snprintf(e1.effective_ver, sizeof(e1.effective_ver), "01.12");
    e1.has_patch = 1;
    e1.dlc_count = 3;
    snprintf(e1.dlc_mode, sizeof(e1.dlc_mode), "external");
    snprintf(e1.state, sizeof(e1.state), "pending_pilot");
    e1.updated_at = 1764240000;
    CHECK(inv_upsert(inv, &e1) == 0, "upsert CUSA00001");

    inv_entry_t e2;
    memset(&e2, 0, sizeof(e2));
    snprintf(e2.title_id, sizeof(e2.title_id), "CUSA00002");
    snprintf(e2.source, sizeof(e2.source), "/mnt/usb1/Games\tWith\nControls/game.pkg");
    snprintf(e2.mount_mode, sizeof(e2.mount_mode), "hold-inplace");
    snprintf(e2.game_ver, sizeof(e2.game_ver), "01.00");
    snprintf(e2.effective_ver, sizeof(e2.effective_ver), "01.00");
    e2.has_patch = 0;
    e2.dlc_count = 0;
    snprintf(e2.dlc_mode, sizeof(e2.dlc_mode), "off");
    snprintf(e2.state, sizeof(e2.state), "registered");
    e2.updated_at = 1764240001;
    CHECK(inv_upsert(inv, &e2) == 0, "upsert CUSA00002");

    CHECK(inv_save(inv, "/system_data/priv/mms/inventory.tsv") == -1,
          "inv_save refuses any path under /system_data/priv/mms");
    CHECK(inv_save(inv, path) == 0, "inv_save writes atomically");
    struct stat st;
    CHECK(stat(tmp_path, &st) != 0, "no leftover .tmp file after atomic rename");

    inv_list_t *reloaded = inv_alloc();
    CHECK(reloaded != NULL, "reloaded alloc");
    CHECK(inv_load(reloaded, path) == 2, "reloaded 2 entries");
    const inv_entry_t *r1 = inv_find(reloaded, "CUSA00001");
    const inv_entry_t *r2 = inv_find(reloaded, "CUSA00002");
    CHECK(r1 != NULL && strcmp(r1->effective_ver, "01.12") == 0 && r1->dlc_count == 3 &&
              strcmp(r1->state, "pending_pilot") == 0,
          "CUSA00001 fields preserved across save/load");
    CHECK(r2 != NULL && strchr(r2->source, '\t') == NULL && strchr(r2->source, '\n') == NULL,
          "control characters sanitized in TSV source path");

    /* Updating CUSA00001 preserves CUSA00002 from another drive. */
    snprintf(e1.state, sizeof(e1.state), "registered");
    CHECK(inv_upsert(reloaded, &e1) == 0, "update CUSA00001 state");
    CHECK(reloaded->count == 2, "count remains 2");
    CHECK(inv_find(reloaded, "CUSA00002") != NULL, "other drive entry preserved");

    inv_free(inv);
    inv_free(reloaded);
    unlink(path);
    rmdir(dir);
}

/* REG-9 / H5: never unmount /user/app while AppInstallAll is still busy. */
static void test_can_unmask_while_busy(void) {
    CHECK(dbguard_can_unmask(0, 0) == 0, "unmasked does not unmask");
    CHECK(dbguard_can_unmask(0, 1) == 0, "unmasked while busy does not unmask");
    CHECK(dbguard_can_unmask(1, 1) == 0, "masked while AppInstallAll busy MUST NOT force-unmount /user/app");
    CHECK(dbguard_can_unmask(1, 0) == 1, "masked once AppInstallAll finished may unmask");
}

int main(void) {
    test_corrupt_marker_startup_and_midbatch();
    test_klog_corruption_during_registration();
    test_first_start_pilot_breaker();
    test_staging_unmount_and_db_error_abort();
    test_can_unmask_while_busy();
    test_operational_inventory();

    printf("dbguard_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
