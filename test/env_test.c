/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * PS4Mount coexistence probe, registration guard, migration guard, and AutoRun tests
 * (specs/ps4-native.md P4N-32, specs/registration.md REG-12). */
#include "autorun.h"
#include "coexist.h"

#include <limits.h>
#include <stdint.h>
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

static void write_bytes(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f)
        exit(2);
    if (fwrite(data, 1, n, f) != n || fclose(f) != 0)
        exit(2);
}

static void make_payload(const char *path, uint8_t m0, uint8_t m1, uint8_t m2, uint8_t m3, size_t len) {
    uint8_t *buf = calloc(1, len);
    if (!buf)
        exit(2);
    if (len >= 4) {
        buf[0] = m0;
        buf[1] = m1;
        buf[2] = m2;
        buf[3] = m3;
    }
    write_bytes(path, buf, len);
    free(buf);
}

typedef struct {
    char root[128];
    char gh_payloads[256];
    char payloads[256];
    char ini[256];
    char user_app[256];
    char psm_state[256];
    char psm_sys[256];
    coexist_paths_t paths;
} env_fixture_t;

static void fixture_init(env_fixture_t *fx) {
    snprintf(fx->root, sizeof(fx->root), "/tmp/lodestar-env-XXXXXX");
    if (!mkdtemp(fx->root))
        exit(2);
    snprintf(fx->gh_payloads, sizeof(fx->gh_payloads), "%s/gh_payloads", fx->root);
    snprintf(fx->payloads, sizeof(fx->payloads), "%s/payloads", fx->root);
    snprintf(fx->ini, sizeof(fx->ini), "%s/payloads.ini", fx->root);
    snprintf(fx->user_app, sizeof(fx->user_app), "%s/user_app", fx->root);
    snprintf(fx->psm_state, sizeof(fx->psm_state), "%s/data_ps4mount", fx->root);
    snprintf(fx->psm_sys, sizeof(fx->psm_sys), "%s/sys_ps4mount", fx->root);
    mkdir(fx->gh_payloads, 0755);
    mkdir(fx->payloads, 0755);
    mkdir(fx->user_app, 0755);
    fx->paths.goldhen_payloads = fx->gh_payloads;
    fx->paths.payloads = fx->payloads;
    fx->paths.payloads_ini = fx->ini;
    fx->paths.user_app = fx->user_app;
    fx->paths.ps4mount_state_dir = fx->psm_state;
    fx->paths.ps4mount_sys_dir = fx->psm_sys;
}

static void test_clean_and_leftover(void) {
    env_fixture_t fx;
    fixture_init(&fx);
    coexist_state_t s;
    coexist_probe(&fx.paths, &s);
    CHECK(!coexist_ps4mount_installed(&s), "clean console: not installed");
    CHECK(!s.ps4mount_autorun, "clean console: not in AutoRun");
    CHECK(!s.ps4mount_state, "clean console: no state");
    CHECK(!coexist_block_auto_register(&s), "clean console: auto-register allowed");

    /* Leftover /data/ps4mount alone is recorded as ps4mount_state without blocking (P4N-32). */
    mkdir(fx.psm_state, 0755);
    coexist_probe(&fx.paths, &s);
    CHECK(!coexist_ps4mount_installed(&s), "leftover /data/ps4mount alone is not installed");
    CHECK(s.ps4mount_state, "leftover /data/ps4mount recorded in ps4mount_state");
    CHECK(!coexist_block_auto_register(&s), "leftover /data/ps4mount alone does not block auto-register");

    /* Too-small or bad-magic file named ps4mount-ps4.bin is ignored. */
    char p[320];
    snprintf(p, sizeof(p), "%s/ps4mount-ps4.bin", fx.gh_payloads);
    make_payload(p, 0x55, 0x48, 0, 0, 128);
    coexist_probe(&fx.paths, &s);
    CHECK(!coexist_ps4mount_installed(&s), "tiny stub ps4mount-ps4.bin ignored");
    make_payload(p, 'N', 'O', 'P', 'E', 8192);
    coexist_probe(&fx.paths, &s);
    CHECK(!coexist_ps4mount_installed(&s), "non-payload magic ps4mount-ps4.bin ignored");
    unlink(p);
}

static void test_installed_and_autorun(void) {
    env_fixture_t fx;
    fixture_init(&fx);
    coexist_state_t s;
    char p[320];

    /* Real .bin payload under /data/GoldHEN/payloads. */
    snprintf(p, sizeof(p), "%s/ps4mount-ps4.bin", fx.gh_payloads);
    make_payload(p, 0x55, 0x48, 0x89, 0xe5, 8192);
    coexist_probe(&fx.paths, &s);
    CHECK(s.ps4mount_payload && coexist_ps4mount_installed(&s), "valid ps4mount-ps4.bin detected");
    CHECK(coexist_block_auto_register(&s), "ps4mount-ps4.bin blocks auto-register");
    unlink(p);

    /* Real .elf payload under /data/payloads. */
    snprintf(p, sizeof(p), "%s/ps4mount-ps4.elf", fx.payloads);
    make_payload(p, 0x7f, 'E', 'L', 'F', 8192);
    coexist_probe(&fx.paths, &s);
    CHECK(s.ps4mount_payload && coexist_block_auto_register(&s), "valid ps4mount-ps4.elf detected");
    unlink(p);

    /* PSMT00001 app directory under /user/app. */
    snprintf(p, sizeof(p), "%s/PSMT00001", fx.user_app);
    mkdir(p, 0755);
    coexist_probe(&fx.paths, &s);
    CHECK(s.ps4mount_app && coexist_block_auto_register(&s), "PSMT00001 blocks auto-register");
    rmdir(p);

    /* PMNT00001 shortcut directory under /user/app. */
    snprintf(p, sizeof(p), "%s/PMNT00001", fx.user_app);
    mkdir(p, 0755);
    coexist_probe(&fx.paths, &s);
    CHECK(s.ps4mount_shortcut && coexist_block_auto_register(&s), "PMNT00001 blocks auto-register");
    rmdir(p);

    /* Commented or disabled (= 0) AutoRun line does NOT block. */
    const char *ini_off = "[AutoRun]\n# /data/payloads/ps4mount-ps4.bin = 1\n/data/payloads/ps4mount-ps4.elf = 0\n";
    write_bytes(fx.ini, ini_off, strlen(ini_off));
    coexist_probe(&fx.paths, &s);
    CHECK(!s.ps4mount_autorun && !coexist_block_auto_register(&s), "commented or =0 AutoRun entry does not block");

    /* Active AutoRun entry DOES block, even after payload file removed. */
    const char *ini_on = "[AutoRun]\n/data/GoldHEN/payloads/ps4mount-ps4.bin = 1\n";
    write_bytes(fx.ini, ini_on, strlen(ini_on));
    coexist_probe(&fx.paths, &s);
    CHECK(s.ps4mount_autorun && coexist_block_auto_register(&s), "active AutoRun entry blocks auto-register");

    /* autorun_edit_ini preserves PS4Mount entry (P4N-19) while inserting Lodestar. */
    buf_t out = {0};
    CHECK(autorun_edit_ini((const uint8_t *)ini_on, strlen(ini_on), "/data/payloads/lodestar-ps4.bin", 1, &out) == 0,
          "autorun_edit_ini succeeds");
    CHECK(out.data && strstr((const char *)out.data, "ps4mount-ps4.bin") != NULL &&
              strstr((const char *)out.data, "lodestar-ps4.bin") != NULL,
          "autorun_edit_ini preserves PS4Mount line (P4N-19)");
    buf_free(&out);

    char js[256];
    size_t n = coexist_format_json(&s, js, sizeof(js));
    CHECK(n > 0 && strstr(js, "\"auto_register_blocked\":true") != NULL && strstr(js, "\"autorun\"") != NULL,
          "coexist_format_json serializes blocked state and reasons");
}

static void test_decisions(void) {
    /* REG-12: blocked auto-register rejects unrequested titles, allows only explicit manual Add. */
    CHECK(coexist_title_register_allowed(0, "CUSA00001", "") == 1, "unblocked allows auto-register");
    CHECK(coexist_title_register_allowed(1, "CUSA00001", "") == 0, "blocked holds automatic registration");
    CHECK(coexist_title_register_allowed(1, "CUSA00001", "CUSA00002") == 0,
          "blocked holds other titles during single-title manual add");
    CHECK(coexist_title_register_allowed(1, "CUSA00001", "CUSA00001") == 1,
          "blocked still allows the explicitly requested manual add title");

    /* P4N-32 / ENV-MIGRATE: migrate_organized is blocked when PS4Mount is present. */
    CHECK(coexist_migrate_allowed(0, 0, 0) == 1, "migrate runs when organize=0, healthy, no PS4Mount");
    CHECK(coexist_migrate_allowed(1, 0, 0) == 0, "migrate skipped when organize=1");
    CHECK(coexist_migrate_allowed(0, 1, 0) == 0, "migrate skipped when db_corrupt");
    CHECK(coexist_migrate_allowed(0, 0, 1) == 0, "migrate skipped when PS4Mount coexist guard active");
}

int main(void) {
    test_clean_and_leftover();
    test_installed_and_autorun();
    test_decisions();
    printf("env_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
