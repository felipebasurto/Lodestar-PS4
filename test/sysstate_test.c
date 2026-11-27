/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Interrupt mount, register, delete and scan, and check what survives.
 * EXPECTED-FAIL is a desired property this tree does not meet yet. The suite
 * still exits 0. If that property starts holding, the test fails so it can be
 * promoted to a normal check. Nothing here runs on a console. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dbsnap.h"
#include "durable.h"
#include "mountplan.h"
#include "stage.h"
#include "sysstate.h"

static int g_failed, g_checks, g_expected;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

/* desired is the safe property. While it is false the hazard is still open. */
#define EXPECT_FAIL(desired, name)                                                                                     \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (desired) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  PROMOTE %s:%d: %s\n", __FILE__, __LINE__, name);                                                 \
        } else {                                                                                                       \
            g_expected++;                                                                                              \
            printf("  EXPECTED-FAIL %s\n", name);                                                                      \
        }                                                                                                              \
    } while (0)

static char *read_whole(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    char *b = malloc((size_t)sz + 1);
    if (!b) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    b[got] = 0;
    if (n)
        *n = got;
    return b;
}

static int has(const char *hay, const char *needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

static int region_has(const char *src, const char *start, const char *end, const char *needle) {
    const char *a = src ? strstr(src, start) : NULL;
    if (!a)
        return 0;
    const char *b = strstr(a + strlen(start), end);
    if (!b)
        return 0;
    size_t n = (size_t)(b - a), m = strlen(needle);
    if (m > n)
        return 0;
    for (size_t i = 0; i + m <= n; i++)
        if (!memcmp(a + i, needle, m))
            return 1;
    return 0;
}

static int slice_has(const char *src, const char *start, size_t window, const char *needle) {
    const char *a = src ? strstr(src, start) : NULL;
    if (!a)
        return 0;
    size_t n = strlen(a);
    if (n > window)
        n = window;
    size_t m = strlen(needle);
    if (m > n)
        return 0;
    for (size_t i = 0; i + m <= n; i++)
        if (!memcmp(a + i, needle, m))
            return 1;
    return 0;
}

static int read_file(const char *path, char *buf, size_t cap, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t got = fread(buf, 1, cap, f);
    fclose(f);
    if (got == cap)
        return -1;
    buf[got] = 0;
    if (n)
        *n = got;
    return 0;
}

static int fail_temp(void) { return 1; }

static int no_tmp_sibling(const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int ok = 1;
    size_t n = strlen(prefix);
    while ((e = readdir(d))) {
        if (!strncmp(e->d_name, prefix, n) && strstr(e->d_name, ".tmp."))
            ok = 0;
    }
    closedir(d);
    return ok;
}

static void test_durable(void) {
    printf("SYS-2: a replace is the previous file or the new file\n");
    const char *dir = "build/test/sysstate-work";
    char path[256], other[256], link[256];
    snprintf(path, sizeof(path), "%s/param.sfo", dir);
    snprintf(other, sizeof(other), "%s/target-bytes", dir);
    snprintf(link, sizeof(link), "%s/via-link", dir);
    CHECK(mkdir("build/test", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir(dir, 0755) == 0 || errno == EEXIST);
    unlink(path);
    unlink(other);
    unlink(link);

    CHECK(durable_replace(path, "NEWFILE!", 8, 0777) == 0);
    char buf[32];
    size_t n = 0;
    CHECK(read_file(path, buf, sizeof(buf), &n) == 0 && n == 8 && !memcmp(buf, "NEWFILE!", 8));
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0777);
    CHECK(no_tmp_sibling(dir, "param.sfo"));

    CHECK(chmod(path, 0640) == 0);
    durable_fail_after_temp = fail_temp;
    CHECK(durable_replace(path, "SHORT", 5, 0777) == -1);
    durable_fail_after_temp = NULL;
    CHECK(read_file(path, buf, sizeof(buf), &n) == 0 && n == 8 && !memcmp(buf, "NEWFILE!", 8));
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640);
    CHECK(no_tmp_sibling(dir, "param.sfo"));

    CHECK(durable_replace(path, "WHOLE-NEW", 9, 0777) == 0);
    CHECK(read_file(path, buf, sizeof(buf), &n) == 0 && n == 9 && !memcmp(buf, "WHOLE-NEW", 9));
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640); /* existing mode kept */

    CHECK(durable_replace(other, "keep-me", 7, 0644) == 0);
    CHECK(symlink("target-bytes", link) == 0);
    CHECK(durable_replace(link, "replaced", 8, 0644) == 0);
    CHECK(read_file(other, buf, sizeof(buf), &n) == 0 && n == 7 && !memcmp(buf, "keep-me", 7));
    CHECK(read_file(link, buf, sizeof(buf), &n) == 0 && n == 8 && !memcmp(buf, "replaced", 8));
    CHECK(lstat(link, &st) == 0 && S_ISREG(st.st_mode));

    char dest[256];
    snprintf(dest, sizeof(dest), "%s/not-a-file", dir);
    CHECK(mkdir(dest, 0755) == 0 || errno == EEXIST);
    CHECK(durable_replace(dest, "x", 1, 0644) == -1);
    CHECK(stat(dest, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(no_tmp_sibling(dir, "not-a-file"));

    for (int existed = 0; existed < 2; existed++) {
        for (int step = 0; step <= 2; step++) {
            sys_bytes_t got = sys_replace_visible(1, existed, step);
            CHECK(got != SYS_BYTES_TRUNC);
            if (step == 1)
                CHECK(sys_replace_leaves_temp(1, step));
        }
        CHECK(sys_replace_visible(0, existed, 1) == SYS_BYTES_TRUNC);
    }
    CHECK(sys_replace_visible(1, 1, 1) == SYS_BYTES_OLD);
    CHECK(sys_replace_visible(1, 0, 1) == SYS_BYTES_ABSENT);
    CHECK(sys_replace_visible(1, 1, 2) == SYS_BYTES_NEW);
}

static void test_snapshot(void) {
    printf("SYS-1: a journal withholds a first read and a shrink\n");
    CHECK(!db_snapshot_publish(0, 0, 10, 1));
    CHECK(db_snapshot_publish(0, 0, 10, 0));
    CHECK(!db_snapshot_publish(1, 10, 9, 1));
    CHECK(!db_snapshot_publish(1, 10, 0, 1));
    CHECK(db_snapshot_publish(1, 10, 9, 0));
    CHECK(db_snapshot_publish(1, 10, 0, 0)); /* a real rebuild, journal already gone */
    CHECK(db_snapshot_publish(1, 10, 10, 1));
    CHECK(db_snapshot_publish(1, 10, 11, 1));
    CHECK(!db_snapshot_publish(1, 10, -1, 0));
    CHECK(db_snapshot_publish(1, 10, 9, 1) == db_snapshot_publish(1, 10, 9, 1));
}

static void test_register_interrupts(void) {
    printf("register: every interrupt keeps saves, the library and a non-truncated file\n");
    for (int step = 0; step < SYS_REG_N; step++) {
        for (int fate = SYS_FATE_STAY; fate <= SYS_FATE_REBUILD; fate++) {
            sys_obs_t o = sys_register_obs(step, (sys_fate_t)fate);
            if (!(o.saves && o.library && o.usb_pkg && !o.db_written))
                printf("  step %s fate %d lost user data\n", sys_reg_name(step), fate);
            CHECK(o.saves && o.library && o.usb_pkg && !o.db_written);
            CHECK(o.meta != SYS_BYTES_TRUNC && o.rif != SYS_BYTES_TRUNC && o.idx != SYS_BYTES_TRUNC);
            if (fate == SYS_FATE_REBOOT || fate == SYS_FATE_REBUILD)
                CHECK(!o.mounted && !o.masked);
            if (fate == SYS_FATE_DEATH) {
                sys_obs_t live = sys_register_obs(step, SYS_FATE_STAY);
                CHECK(o.mounted == live.mounted && o.masked == live.masked);
            }
            if (fate == SYS_FATE_REBUILD) {
                CHECK(!o.db_row);
                if (step >= SYS_REG_META)
                    CHECK(o.meta == SYS_BYTES_NEW && o.appmeta && o.playgo);
            }
        }
        sys_obs_t again = sys_register_obs(step, SYS_FATE_STAY);
        CHECK(again.library && again.saves && !again.db_written);
    }
    sys_obs_t temp = sys_register_obs(SYS_REG_META_TEMP, SYS_FATE_DEATH);
    CHECK(temp.meta == SYS_BYTES_ABSENT && temp.orphan_temp && !temp.db_row);
    sys_obs_t mid = sys_register_obs(SYS_REG_LIC_RIF, SYS_FATE_REBOOT);
    CHECK(!sys_pair_consistent(mid.rif, mid.idx));
    CHECK(mid.appmeta && mid.playgo && !mid.db_row && !mid.mounted);
    sys_obs_t masked = sys_register_obs(SYS_REG_MASKED, SYS_FATE_DEATH);
    CHECK(masked.masked && masked.mounted && !masked.db_row);
    sys_obs_t rebooted = sys_register_obs(SYS_REG_MASKED, SYS_FATE_REBOOT);
    CHECK(!rebooted.masked && !rebooted.mounted && rebooted.empty_user_dir);
    sys_obs_t done = sys_register_obs(SYS_REG_MARKER, SYS_FATE_REBOOT);
    CHECK(done.db_row && done.marker_src && !done.mounted);
    CHECK(sys_pair_consistent(done.rif, done.idx));
    sys_obs_t rebuilt = sys_register_obs(SYS_REG_MARKER, SYS_FATE_REBUILD);
    CHECK(!rebuilt.db_row && rebuilt.marker_src && rebuilt.hold && rebuilt.usb_pkg);
}

static void test_delete_and_scan(void) {
    printf("delete and scan: library loss only on a bound commit\n");
    for (int step = 0; step < SYS_DEL_N; step++) {
        for (int fate = SYS_FATE_STAY; fate <= SYS_FATE_REBUILD; fate++) {
            sys_obs_t o = sys_delete_obs(step, 1, (sys_fate_t)fate);
            CHECK(o.saves && !o.db_written);
        }
        int commits = sys_delete_commits_now(step);
        int should = sys_delete_commit_should(step);
        sys_obs_t org = sys_delete_obs(step, 1, SYS_FATE_STAY);
        sys_obs_t inplace = sys_delete_obs(step, 0, SYS_FATE_STAY);
        if (step == SYS_DEL_UNBOUND_ZERO) {
            EXPECT_FAIL(commits == should, "H1 unbound uninstall result must not commit the library delete (follow-up)");
            CHECK(!org.library && !org.usb_pkg); /* current: organized folder is deleted */
            CHECK(inplace.usb_pkg && inplace.marker_removed);
        } else {
            CHECK(commits == should);
            if (should)
                CHECK(!org.library && !org.usb_pkg);
            else
                CHECK(org.library && org.usb_pkg);
        }
    }
    /* Power loss before a commit keeps the folder: the pending flag is memory only. */
    sys_obs_t died = sys_delete_obs(SYS_DEL_INTENT, 1, SYS_FATE_DEATH);
    CHECK(died.library && died.usb_pkg && died.saves);
    sys_obs_t timed = sys_delete_obs(SYS_DEL_TIMEOUT, 1, SYS_FATE_STAY);
    CHECK(timed.library && timed.usb_pkg);

    CHECK(!sys_scan_may_register(0, 0));
    CHECK(!sys_scan_may_register(0, 1));
    CHECK(sys_scan_may_register(1, 0));
    CHECK(sys_scan_should_register(1, 0));
    CHECK(!sys_scan_should_register(1, 1));

    CHECK(sys_addcont_installed_now(1));
    CHECK(!sys_addcont_installed_now(0));
    CHECK(!sys_addcont_installed_should(0, 0));
    CHECK(sys_addcont_installed_should(1, 1));
}

static void test_idempotent_boundaries(void) {
    printf("idempotent boundaries that already hold\n");
    CHECK(mount_prepare_step(1, 1, 1, 1, 1, 1) == MOUNT_PREPARE_ALREADY);
    CHECK(mount_prepare_step(1, 1, 1, 1, 1, 0) == MOUNT_PREPARE_ALREADY); /* exact top, twice */
    CHECK(mount_detach_step(1, 0, 0, 0, 0) == MOUNT_STEP_DONE);
    CHECK(mount_detach_step(1, 0, 0, 0, 0) == MOUNT_STEP_DONE);
    CHECK(!stage_installed_ok(1, 1, 1, 1, 10, 9));
    CHECK(!stage_installed_ok(1, 0, 1, 1, 10, 10));
    CHECK(!stage_installed_ok(1, 1, 0, 1, 10, 10));
    CHECK(stage_installed_ok(1, 1, 1, 1, 10, 10));
    CHECK(delete_target_ok("/mnt/usb0", "CUSA00001", "/mnt/usb0/ps4/CUSA00001"));
    CHECK(!delete_target_ok("/mnt/usb0", "CUSA00001", "/mnt/usb0/CUSA00001"));
    CHECK(!delete_target_ok("/mnt/usb0", "CUSA00001", "/user/app/CUSA00001"));
    CHECK(delete_pending_decision(-1, 1, 1, 0, DELETE_PENDING_MAX_SEC) == -1);
    CHECK(delete_pending_decision(0, 1, 0, 0, 0) == 1); /* current DEL-6, title not required */
}

static void test_open_followups(const char *main_src) {
    printf("open hazards (expected-fail until the owning change lands)\n");
    EXPECT_FAIL(region_has(main_src, "AppUnInstallMainExecute ret = [", "system file disagree", "is_game_id") ||
                    region_has(main_src, "AppUnInstallMainExecute ret = [", "system file disagree", "strncmp") ||
                    region_has(main_src, "AppUnInstallMainExecute ret = [", "system file disagree", "strcmp"),
                "H1 uninstall result must name the pending title (#15-#18 do not; follow-up)");

    int enforce_writes = region_has(main_src, "static void enforce_patch_sfo", "Registration method:", "write_file(path, sfo");
    int hidden_internal = has(main_src, "if (mounted_from(p, NULL, 0))\n        return 0;");
    EXPECT_FAIL(!(enforce_writes && hidden_internal),
                "H2 do not rewrite priv param.sfo while a mount hides a real app.pkg (#15 reads a larger sfo only; follow-up)");

    EXPECT_FAIL(!has(main_src, "if (place_license(app))\n        return -1;"),
                "H3 a failed register must roll back appmeta, license and playgo (follow-up)");

    EXPECT_FAIL(region_has(main_src, "static void shutdown_daemon(int lock)", "static int single_instance", "unmask"),
                "H4 shutdown must unmask /user/app (follow-up; #15/#16 fix unmask itself)");

    int awake = region_has(main_src, "static void keep_awake(void)", "static void pump_events", "g_install_busy") ||
                region_has(main_src, "static void keep_awake(void)", "static void pump_events", "install_waiting") ||
                region_has(main_src, "static void keep_awake(void)", "static void pump_events", "dlc");
    EXPECT_FAIL(awake, "H4 rest must stay off during AppInstallAll and DLC copy (follow-up)");

    EXPECT_FAIL(!has(main_src, "write_file(path, tid, strlen(tid), 0666);\n    unlink(srcpath);\n"
                               "    int rc = db_is_registered(tid) ? uninstall_title(tid) : 0;"),
                "H4 .removed must be written only after uninstall returns (follow-up)");

    EXPECT_FAIL(!has(main_src, "int rc = write_buf(rp, &rif) | write_buf(ip, &idx);"),
                "H5 rif and idx must commit together (single-file truncate is SYS-2; the pair is a follow-up)");

    EXPECT_FAIL(!has(main_src, "for (int d1 = 0; d1 < 2; d1++)"),
                "H5 BGFT d0.pdb and d1.pdb must commit together (follow-up; BGFT reads them at boot)");

    EXPECT_FAIL(!has(main_src, "    if (user_app_masked())\n"
                               "        printf(\"[reg] /user/app still shows only the new games\\n\");\n"
                               "    g_masked = 0;\n"
                               "    stage_clear();"),
                "unmask must keep g_masked while /user/app is still masked (#15 and #16)");

    EXPECT_FAIL(!has(main_src, "for (int i = 0; masked && __atomic_load_n(&g_install_busy, __ATOMIC_ACQUIRE) && i < 120; i++)\n"
                               "        pump_events(1000);\n"
                               "    if (masked)\n"
                               "        unmask_user_app(tids, n);\n"
                               "    if (__atomic_load_n(&g_install_busy, __ATOMIC_ACQUIRE)) {\n"
                               "        printf(\"[reg] %s still running:"),
                "install_all must not unmask and continue while AppInstallAll is still busy (#16)");

    EXPECT_FAIL(!has(main_src, "            int32_t user = 0;\n"
                               "            sceUserServiceGetForegroundUser(&user);\n"
                               "            lnc_param_t lp = {sizeof(lp), (uint32_t)user, 0, 0, 0};"),
                "panel launch must reject foreground user 0 (#15 and #16)");

    EXPECT_FAIL(!has(main_src, "    int32_t user = 0;\n"
                               "    sceUserServiceGetForegroundUser(&user);\n"
                               "    char kind[24];"),
                "DLC install must reject foreground user 0 (#15 and #16)");

    EXPECT_FAIL(!has(main_src, "    int32_t user = 0;\n"
                               "    sceUserServiceGetForegroundUser(&user);\n"
                               "    int task = install_pkg(pkg_url(tid[0] ? tid : \"install\", \"up\", path, url, sizeof(url)), title, user, msg,"),
                "received-pkg install must reject foreground user 0 (#15 and #16)");

    EXPECT_FAIL(!has(main_src, "    if (sysctl(mib, 4, NULL, &len, NULL, 0) || !len)\n        return 0;"),
                "open_apps must not report \"nothing is open\" when the process list cannot be read (#15 and #16)");

    EXPECT_FAIL(!has(main_src, "printf(\"[main] first start: nobody at the panel, the default settings are saved\\n\");\n"
                               "                config_save();"),
                "first start must not save defaults and scan when the panel is idle (#15 and #16)");

    int scan_stops = slice_has(main_src, "static int scan_all(int hard)", 4000, "corrupt") ||
                     slice_has(main_src, "static int scan_all(int hard)", 4000, "dbguard_halt");
    EXPECT_FAIL(scan_stops, "scan_all must stop when app.db is marked corrupt (#15 diagnostics, #16 halt)");

    EXPECT_FAIL(has(main_src, "pilot_tid") || has(main_src, "BREAKER_PILOT"),
                "after a rebuild, registration must be capped (one pilot) before the rest (#16; #15 is first-start only)");

    EXPECT_FAIL(region_has(main_src, "static int addcont_installed", "static int mount_paths", "st_size"),
                "a partial /user/addcont directory must not count as a finished install (follow-up)");
}

int main(void) {
    test_durable();
    test_snapshot();
    test_register_interrupts();
    test_delete_and_scan();
    test_idempotent_boundaries();
    char *src = read_whole("src/main.c", NULL);
    CHECK(src != NULL);
    if (src)
        test_open_followups(src);
    free(src);
    CHECK(g_expected == 19);
    printf("%d checks, %d failed, %d expected-fail\n", g_checks, g_failed, g_expected);
    return g_failed != 0;
}
