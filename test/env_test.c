/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Environment the payload runs in: PS4Mount beside Lodestar, the shared lock,
 * a second load, and an upgrade from older settings. EXPECTED-FAIL marks a
 * hazard whose fix belongs in another change; the suite still exits 0. */
#include "autorun.h"
#include "coexist.h"
#include "fw.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks, g_failed, g_xfail;

#define CHECK(c)                                                                                       \
    do {                                                                                               \
        g_checks++;                                                                                    \
        if (!(c)) {                                                                                    \
            g_failed++;                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                        \
        }                                                                                              \
    } while (0)

/* desired is the safe condition. It is still false on this branch: print
 * EXPECTED-FAIL and keep going. If it becomes true, the mark is stale. */
#define XFAIL(id, desired, owner, msg)                                                                 \
    do {                                                                                               \
        g_checks++;                                                                                    \
        if (desired) {                                                                                 \
            g_failed++;                                                                                \
            printf("STALE-XFAIL %s (%s): the bug is gone, remove the expected-fail: %s\n", id, owner,  \
                   msg);                                                                               \
        } else {                                                                                       \
            g_xfail++;                                                                                 \
            printf("EXPECTED-FAIL %s (%s): %s\n", id, owner, msg);                                     \
        }                                                                                              \
    } while (0)

void buf_put(buf_t *b, const void *data, size_t len) {
    if (!b || !len)
        return;
    uint8_t *p = realloc(b->data, b->len + len + 1);
    if (!p)
        return;
    memcpy(p + b->len, data, len);
    b->len += len;
    p[b->len] = 0;
    b->data = p;
    b->cap = b->len + 1;
}

void buf_free(buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    char buf[8192];
    size_t nlen, fill = 0;
    if (!f || !needle)
        return 0;
    nlen = strlen(needle);
    while (fill < sizeof(buf)) {
        size_t n = fread(buf + fill, 1, sizeof(buf) - fill, f);
        fill += n;
        if (fill >= nlen) {
            for (size_t i = 0; i + nlen <= fill; i++)
                if (!memcmp(buf + i, needle, nlen)) {
                    fclose(f);
                    return 1;
                }
            if (nlen > 1 && fill > nlen - 1) {
                memmove(buf, buf + fill - (nlen - 1), nlen - 1);
                fill = nlen - 1;
            }
        }
        if (!n)
            break;
    }
    fclose(f);
    return 0;
}

static void write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(fwrite(data, 1, n, f) == n);
    fclose(f);
}

static void test_decisions(void) {
    printf("ps4mount present, autorun, already registered\n");
    coexist_evidence_t none;
    memset(&none, 0, sizeof(none));
    CHECK(!coexist_block_auto_register(&none));
    CHECK(!coexist_ps4mount_installed(&none));

    coexist_evidence_t app;
    memset(&app, 0, sizeof(app));
    app.app_installed = 1;
    CHECK(coexist_ps4mount_installed(&app));
    CHECK(coexist_block_auto_register(&app));

    coexist_evidence_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.payload_file = 1;
    CHECK(coexist_block_auto_register(&payload));

    coexist_evidence_t cut;
    memset(&cut, 0, sizeof(cut));
    cut.shortcut = 1;
    CHECK(coexist_block_auto_register(&cut));

    coexist_evidence_t ar;
    memset(&ar, 0, sizeof(ar));
    ar.autorun = 1;
    CHECK(!coexist_ps4mount_installed(&ar));
    CHECK(coexist_block_auto_register(&ar));

    /* A title PS4Mount already put in app.db still fails Lodestar's own checks:
     * the BGFT task names the other hold path, and the .src is under /data/ps4mount. */
    CHECK(coexist_reregister_why(1, 0, 0, 0, 1, 0, 1, 1) == COEXIST_WHY_BGFT_URL);
    CHECK(coexist_reregister_why(1, 1, 0, 0, 1, 0, 1, 1) == COEXIST_WHY_MARKER);
    CHECK(coexist_reregister_why(0, 1, 0, 1, 1, 0, 1, 1) == COEXIST_WHY_NOT_REGISTERED);
    CHECK(coexist_reregister_why(1, 1, 1, 1, 1, 0, 1, 1) == COEXIST_WHY_BGFT_ERROR);
    CHECK(coexist_reregister_why(1, 1, 0, 1, 0, 0, 1, 1) == COEXIST_WHY_GAME_LINK);
    CHECK(coexist_reregister_why(1, 1, 0, 1, 1, 1, 0, 1) == COEXIST_WHY_UPDATE_LINK);
    CHECK(coexist_reregister_why(1, 1, 0, 1, 1, 0, 1, 0) == COEXIST_WHY_ADDONS);
    CHECK(coexist_reregister_why(1, 1, 0, 1, 1, 0, 1, 1) == COEXIST_WHY_NONE);

    coexist_why_t whys[] = {COEXIST_WHY_NOT_REGISTERED, COEXIST_WHY_BGFT_URL, COEXIST_WHY_BGFT_ERROR,
                            COEXIST_WHY_MARKER,        COEXIST_WHY_GAME_LINK, COEXIST_WHY_UPDATE_LINK,
                            COEXIST_WHY_ADDONS};
    for (size_t i = 0; i < sizeof(whys) / sizeof(whys[0]); i++) {
        CHECK(coexist_scan_registers(&none, whys[i]) == 1);
        CHECK(coexist_scan_registers(&app, whys[i]) == 0);
        CHECK(coexist_scan_registers(&ar, whys[i]) == 0);
    }
    CHECK(coexist_scan_registers(&none, COEXIST_WHY_NONE) == 0);
    CHECK(coexist_scan_registers(&app, COEXIST_WHY_NONE) == 0);

    /* The panel's Add stays available. That is a residual hazard, not a bug in the guard. */
    CHECK(coexist_manual_register_allowed(&app) == 1);
    CHECK(coexist_manual_register_allowed(&ar) == 1);

    coexist_evidence_t left;
    memset(&left, 0, sizeof(left));
    left.state_dir = 1;
    CHECK(!coexist_block_auto_register(&left));
    CHECK(coexist_scan_registers(&left, COEXIST_WHY_MARKER) == 1);
    XFAIL("ENV-LEFTOVER", coexist_block_auto_register(&left) == 1, "open",
          "leftover /data/ps4mount alone does not block auto-register; P4N-32 is installed or AutoRun only");
}

static void test_ini(void) {
    printf("payloads.ini\n");
    const char *hit = "[AutoRun]\n/data/GoldHEN/payloads/ps4mount-ps4.bin = 1\n";
    CHECK(coexist_ini_has_ps4mount(hit, strlen(hit)));
    const char *elf = "PS4MOUNT-PS4.ELF=1\n";
    CHECK(coexist_ini_has_ps4mount(elf, strlen(elf)));
    const char *comment = "# /data/GoldHEN/payloads/ps4mount-ps4.bin = 1\n;ps4mount-ps4.elf = 1\n";
    CHECK(!coexist_ini_has_ps4mount(comment, strlen(comment)));
    const char *alike =
        "myps4mount-ps4.bin = 1\nps4mount-ps4.binfoo = 1\nps4mount-ps4.bin.bak = 1\n"
        "backup = /data/GoldHEN/payloads/ps4mount-ps4.bin\n"
        "/data/payloads/lodestar-ps4.bin = 1\n";
    CHECK(!coexist_ini_has_ps4mount(alike, strlen(alike)));

    /* Enabling Lodestar inserts its line first and leaves the PS4Mount line. */
    const char *raw = "[AutoRun]\n/data/GoldHEN/payloads/ps4mount-ps4.bin = 1\n";
    buf_t b = {0};
    CHECK(autorun_edit_ini((const uint8_t *)raw, strlen(raw), "/data/payloads/lodestar-ps4.bin", 1, &b) == 0);
    CHECK(b.data != NULL);
    CHECK(coexist_ini_has_ps4mount((const char *)b.data, b.len));
    const char *lodestar = strstr((const char *)b.data, "lodestar-ps4.bin");
    const char *foreign = strstr((const char *)b.data, "ps4mount-ps4.bin");
    CHECK(lodestar && foreign && lodestar < foreign);
    buf_free(&b);
}

static void test_probe(void) {
    printf("probe a tree\n");
    char tmpl[] = "/tmp/lodestar-coexist-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (!dir)
        return;
    char payload[256], ini[256], app[256], state[256], tiny[256], comment_ini[256];
    snprintf(payload, sizeof(payload), "%s/ps4mount-ps4.bin", dir);
    snprintf(ini, sizeof(ini), "%s/payloads.ini", dir);
    snprintf(app, sizeof(app), "%s/PSMT00001", dir);
    snprintf(state, sizeof(state), "%s/ps4mount", dir);
    snprintf(tiny, sizeof(tiny), "%s/tiny.bin", dir);
    snprintf(comment_ini, sizeof(comment_ini), "%s/comment.ini", dir);

    char body[4096];
    memset(body, 0, sizeof(body));
    memcpy(body, "\177ELF", 4);
    write_file(payload, body, sizeof(body));
    write_file(tiny, "UH", 2);
    write_file(ini, "/data/GoldHEN/payloads/ps4mount-ps4.bin = 1\n", 44);
    write_file(comment_ini, "# ps4mount-ps4.bin = 1\n", 23);
    CHECK(mkdir(app, 0777) == 0);
    CHECK(mkdir(state, 0777) == 0);

    const char *payloads[] = {tiny, payload, NULL};
    const char *apps[] = {app, NULL};
    const char *states[] = {state, NULL};
    const char *inis[] = {ini, NULL};
    const char *comments[] = {comment_ini, NULL};
    coexist_probe_paths_t paths = {payloads, apps, NULL, states, inis};
    coexist_evidence_t e;
    coexist_probe(&paths, &e);
    CHECK(e.payload_file && e.app_installed && e.state_dir && e.autorun && !e.shortcut);
    CHECK(coexist_block_auto_register(&e));

    paths.payloads = (const char *const[]){tiny, NULL};
    paths.apps = NULL;
    paths.states = NULL;
    paths.inis = comments;
    coexist_probe(&paths, &e);
    CHECK(!e.payload_file && !e.app_installed && !e.autorun && !e.state_dir);
    CHECK(!coexist_block_auto_register(&e));

    body[0] = 'U';
    body[1] = 'H';
    write_file(payload, body, sizeof(body));
    paths.payloads = (const char *const[]){payload, NULL};
    paths.inis = NULL;
    coexist_probe(&paths, &e);
    CHECK(e.payload_file);

    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    CHECK(system(cmd) == 0);
}

static void test_lock(void) {
    printf("9032 lock\n");
    CHECK(COEXIST_LOCK_PORT == 9032);
    CHECK(COEXIST_PANEL_PORT == 8780);
    CHECK(COEXIST_RELAY_PORT == 8781);
    CHECK(COEXIST_KLOG_PORT == 3232);
    CHECK(COEXIST_PS4MOUNT_PANEL_PORT == 8770);
    CHECK(COEXIST_PS4MOUNT_RELAY_PORT == 8771);
    CHECK(COEXIST_PANEL_PORT != COEXIST_PS4MOUNT_PANEL_PORT);
    CHECK(COEXIST_RELAY_PORT != COEXIST_PS4MOUNT_RELAY_PORT);
    CHECK(COEXIST_LOCK_PORT != COEXIST_PANEL_PORT);
    CHECK(COEXIST_LOCK_PORT != COEXIST_KLOG_PORT);
    CHECK(strcmp(COEXIST_APP_ID, "PSMT00001") == 0);
    CHECK(strcmp(COEXIST_SHORTCUT_ID, "PMNT00001") == 0);
    CHECK(strcmp(COEXIST_PANEL_ID, "LODS00001") == 0);

    /* The interlock is bind without SO_REUSEADDR. An ephemeral port stands in
     * for 9032 so a busy CI port does not fail the suite. */
    int a = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(a >= 0);
    CHECK(bind(a, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(listen(a, 1) == 0);
    socklen_t alen = sizeof(addr);
    CHECK(getsockname(a, (struct sockaddr *)&addr, &alen) == 0);
    int b = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(b >= 0);
    CHECK(bind(b, (struct sockaddr *)&addr, sizeof(addr)) != 0);
    close(b);
    close(a);
    /* A holder of the lock means this load does not start. */
    CHECK(coexist_rest_lost_lock_stops(0, 0) == 1);
    CHECK(coexist_rest_lost_lock_stops(1, 0) == 0);
    CHECK(coexist_rest_lost_lock_stops(0, 1) == 0);
}

static void test_timeout_upgrade_clock(void) {
    printf("timeout, upgrade, clock, double load\n");
    /* The guard swallows the wait while PS4Mount is in the way. */
    CHECK(coexist_timeout_saves(1, 0, 0, 120, 120, 1) == 0);
    CHECK(coexist_timeout_saves(1, 0, 0, 100000, 120, 1) == 0);
    CHECK(coexist_setup_timeout_due(0, 0, 119, 120) == 0);
    CHECK(coexist_setup_timeout_due(0, 0, 120, 120) == 1);
    /* Without the guard, main still saves at 120s and a clock step fires it at once. */
    XFAIL("ENV-TIMEOUT", coexist_timeout_saves(1, 0, 0, 120, 120, 0) == 0, "#15 #16",
          "the 120s first-start wait still saves defaults and the next scan can register");
    XFAIL("ENV-CLOCK", coexist_timeout_saves(1, 0, 0, 100000, 120, 0) == 0, "#15 #16",
          "a forward wall-clock step expires SETUP_WAIT immediately");

    CHECK(coexist_first_start(0, 0) == 1);
    CHECK(coexist_first_start(1, 0) == 0);
    CHECK(coexist_first_start(0, 1) == 0);
    CHECK(coexist_upgrade_organize(1, 0) == 1);
    CHECK(coexist_upgrade_organize(0, 0) == 1);
    CHECK(coexist_upgrade_organize(2, 0) == 0);
    CHECK(coexist_upgrade_organize(3, 1) == 1);
    CHECK(coexist_upgrade_forces_external(2, 0, 1) == 1);
    CHECK(coexist_upgrade_forces_external(2, 0, 0) == 0);
    CHECK(coexist_upgrade_forces_external(3, 0, 1) == 0);

    /* Mark written at wall 1000, monotonic 100, so boot was 900. A +3600 step
     * moves boot to 4500 and the 60s window drops the mark. */
    CHECK(coexist_boot_time(1000, 100) == 900);
    CHECK(coexist_restart_mark_fresh(900, 900) == 1);
    CHECK(coexist_restart_mark_fresh(900, coexist_boot_time(1000 + 3600, 100)) == 0);
    XFAIL("ENV-RESTART-CLOCK",
          coexist_restart_mark_fresh(900, coexist_boot_time(1000 + 3600, 100)) == 1, "open",
          "a wall step over 60s makes needs_reboot drop the .restart mark");

    XFAIL("ENV-DOUBLE-LOAD", coexist_same_process_reentry_safe(1) == 1, "open",
          "a second main() in the same process still calls log_start and, on lock failure, log_stop");
    CHECK(coexist_second_load_stops_log(1) == 1);
    CHECK(coexist_second_load_stops_log(0) == 1);

    XFAIL("ENV-MIGRATE", coexist_migrate_runs(0, 0, 1) == 0, "open",
          "organize off still moves <drive>/ps4 while PS4Mount is present");
    CHECK(coexist_migrate_runs(1, 0, 1) == 0);
    CHECK(coexist_migrate_runs(0, 1, 0) == 0);
}

static void test_firmware(void) {
    printf("firmware 13 and 14\n");
    CHECK(fw_reg_mode(13, 0) == REG_ALL);
    CHECK(fw_reg_mode(14, 0) == REG_ALL);
    CHECK(fw_reg_mode(13, 1) == fw_reg_mode(14, 1));
    CHECK(fw_reg_mode(5, 1) != REG_ALL);
}

static void test_notice_contract(void) {
    printf("notice text\n");
    CHECK(strlen(COEXIST_NOTICE_EN) < 256);
    CHECK(strlen(COEXIST_NOTICE_ES) < 256);
    CHECK(strstr(COEXIST_NOTICE_EN, "not adding games") != NULL);
    CHECK(strstr(COEXIST_NOTICE_ES, "no añade juegos") != NULL);
    CHECK(contains("src/main.c", COEXIST_NOTICE_EN));
    CHECK(contains("src/main.c", COEXIST_NOTICE_ES));
    CHECK(contains("web/index.html", COEXIST_NOTICE_EN));
    CHECK(contains("web/legacy.html", COEXIST_NOTICE_EN));
    CHECK(contains("web/lang.js", COEXIST_NOTICE_ES));
    char js[256];
    coexist_format_json(1, 1, 0, 1, js, sizeof(js));
    CHECK(strcmp(js, "\"coexist\":{\"ps4mount_installed\":true,\"ps4mount_autorun\":true,"
                     "\"ps4mount_state\":false,\"block\":true}") == 0);
    coexist_format_json(0, 0, 1, 0, js, sizeof(js));
    CHECK(strstr(js, "\"ps4mount_state\":true") != NULL);
    CHECK(strstr(js, "\"block\":false") != NULL);
}

int main(void) {
    test_decisions();
    test_ini();
    test_probe();
    test_lock();
    test_timeout_upgrade_clock();
    test_firmware();
    test_notice_contract();
    printf("env_test: %d checks, %d failed, %d expected-fail\n", g_checks, g_failed, g_xfail);
    return g_failed ? 1 : 0;
}
