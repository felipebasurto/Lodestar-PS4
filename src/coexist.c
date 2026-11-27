/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Whether the upstream PS4Mount port is installed or set to start with the console.
 * Lodestar began as a port of gasb07/PS4Mount and keeps that project's license. */
#include "coexist.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(COEXIST_NOTICE_EN) <= 256, "English notice fits the console buffer");
_Static_assert(sizeof(COEXIST_NOTICE_ES) <= 256, "Spanish notice fits the console buffer");

static const char *const k_payloads[] = {
    "/data/payloads/ps4mount-ps4.bin",
    "/data/payloads/PS4Mount-PS4.bin",
    "/data/GoldHEN/payloads/ps4mount-ps4.bin",
    "/data/GoldHEN/payloads/PS4Mount-PS4.bin",
    "/user/data/payloads/ps4mount-ps4.bin",
    "/user/data/payloads/PS4Mount-PS4.bin",
    "/user/data/GoldHEN/payloads/ps4mount-ps4.bin",
    "/user/data/GoldHEN/payloads/PS4Mount-PS4.bin",
    "/data/payloads/ps4mount-ps4.elf",
    "/data/payloads/PS4Mount-PS4.elf",
    "/data/GoldHEN/payloads/ps4mount-ps4.elf",
    "/data/GoldHEN/payloads/PS4Mount-PS4.elf",
    "/user/data/payloads/ps4mount-ps4.elf",
    "/user/data/payloads/PS4Mount-PS4.elf",
    "/user/data/GoldHEN/payloads/ps4mount-ps4.elf",
    "/user/data/GoldHEN/payloads/PS4Mount-PS4.elf",
    NULL};

static const char *const k_apps[] = {
    "/user/app/" COEXIST_APP_ID,
    "/system_data/priv/appmeta/" COEXIST_APP_ID,
    "/user/appmeta/" COEXIST_APP_ID,
    NULL};

static const char *const k_shortcuts[] = {
    "/user/app/" COEXIST_SHORTCUT_ID,
    "/system_data/priv/appmeta/" COEXIST_SHORTCUT_ID,
    "/user/appmeta/" COEXIST_SHORTCUT_ID,
    NULL};

static const char *const k_states[] = {
    "/data/ps4mount",
    "/system_data/ps4mount",
    "/user/data/ps4mount",
    NULL};

static const char *const k_inis[] = {
    "/data/GoldHEN/payloads.ini",
    "/user/data/GoldHEN/payloads.ini",
    NULL};

static const coexist_probe_paths_t k_console = {
    k_payloads, k_apps, k_shortcuts, k_states, k_inis};

int coexist_ps4mount_installed(const coexist_evidence_t *e) {
    return e && (e->payload_file || e->app_installed || e->shortcut);
}

int coexist_block_auto_register(const coexist_evidence_t *e) {
    return coexist_ps4mount_installed(e) || (e && e->autorun);
}

/* Same shape as autorun_is_ours, for the foreign payload names. A commented
 * line, a look-alike and a path after '=' do not count. */
static int line_is_ps4mount(const char *line, size_t len) {
    static const char *const names[] = {"ps4mount-ps4.bin", "ps4mount-ps4.elf"};
    size_t a = 0;
    while (a < len && (line[a] == ' ' || line[a] == '\t'))
        a++;
    if (a < len && (line[a] == '#' || line[a] == ';'))
        return 0;
    size_t base = a, end = a;
    while (end < len && line[end] != '=') {
        if (line[end] == '/')
            base = end + 1;
        end++;
    }
    while (end > base && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                          line[end - 1] == '\r' || line[end - 1] == '\n'))
        end--;
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
        if (strlen(names[k]) == end - base && !strncasecmp(line + base, names[k], end - base))
            return 1;
    return 0;
}

int coexist_ini_has_ps4mount(const char *text, size_t len) {
    if (!text)
        return 0;
    for (size_t pos = 0; pos < len;) {
        size_t e = pos;
        while (e < len && text[e] != '\n')
            e++;
        if (line_is_ps4mount(text + pos, e - pos))
            return 1;
        pos = e < len ? e + 1 : e;
    }
    return 0;
}

static int is_dir(const char *path) {
    struct stat st;
    return path && !stat(path, &st) && S_ISDIR(st.st_mode);
}

/* A loader payload, not a name that happens to match: regular file, at least
 * 4 KiB, and the ELF or UH magic payload_file accepts for Lodestar's own file. */
static int is_payload(const char *path) {
    struct stat st;
    uint8_t d[8];
    int fd;
    ssize_t n;
    if (!path || stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 4096)
        return 0;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, d, sizeof(d));
    close(fd);
    if (n < 4)
        return 0;
    return !memcmp(d, "\177ELF", 4) || (d[0] == 'U' && d[1] == 'H');
}

static int ini_file_has(const char *path) {
    FILE *f;
    char line[1024];
    int hit = 0;
    if (!path)
        return 0;
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (!hit && fgets(line, sizeof(line), f))
        hit = line_is_ps4mount(line, strlen(line));
    fclose(f);
    return hit;
}

static int any_dir(const char *const *paths) {
    if (!paths)
        return 0;
    for (size_t i = 0; paths[i]; i++)
        if (is_dir(paths[i]))
            return 1;
    return 0;
}

void coexist_probe(const coexist_probe_paths_t *paths, coexist_evidence_t *out) {
    memset(out, 0, sizeof(*out));
    if (!paths)
        return;
    if (paths->payloads) {
        for (size_t i = 0; paths->payloads[i]; i++)
            if (is_payload(paths->payloads[i])) {
                out->payload_file = 1;
                break;
            }
    }
    out->app_installed = any_dir(paths->apps);
    out->shortcut = any_dir(paths->shortcuts);
    out->state_dir = any_dir(paths->states);
    if (paths->inis) {
        for (size_t i = 0; paths->inis[i]; i++)
            if (ini_file_has(paths->inis[i])) {
                out->autorun = 1;
                break;
            }
    }
}

const coexist_probe_paths_t *coexist_console_paths(void) { return &k_console; }

coexist_why_t coexist_reregister_why(int db_registered, int bgft_names_pkg, int bgft_error,
                                      int marker_matches, int game_link, int has_patch,
                                      int update_link, int addons_ready) {
    if (!db_registered)
        return COEXIST_WHY_NOT_REGISTERED;
    if (!bgft_names_pkg)
        return COEXIST_WHY_BGFT_URL;
    if (bgft_error)
        return COEXIST_WHY_BGFT_ERROR;
    if (!marker_matches)
        return COEXIST_WHY_MARKER;
    if (!game_link)
        return COEXIST_WHY_GAME_LINK;
    if (has_patch && !update_link)
        return COEXIST_WHY_UPDATE_LINK;
    if (!addons_ready)
        return COEXIST_WHY_ADDONS;
    return COEXIST_WHY_NONE;
}

int coexist_scan_registers(const coexist_evidence_t *e, coexist_why_t why) {
    if (why == COEXIST_WHY_NONE)
        return 0;
    if (coexist_block_auto_register(e))
        return 0;
    return 1;
}

int coexist_manual_register_allowed(const coexist_evidence_t *e) {
    (void)e;
    return 1;
}

int coexist_setup_timeout_due(long start, long seen, long now, long wait) {
    long base = seen > start ? seen : start;
    return now >= base + wait;
}

int coexist_timeout_saves(int setup, long start, long seen, long now, long wait, int block) {
    if (!setup || block)
        return 0;
    return coexist_setup_timeout_due(start, seen, now, wait);
}

int coexist_upgrade_organize(int ver, int file_organize) {
    if (ver < 2)
        return 1;
    return file_organize ? 1 : 0;
}

int coexist_upgrade_forces_external(int ver, int dlc_mode_seen, int dlc_on) {
    return ver < 3 && !dlc_mode_seen && dlc_on;
}

int coexist_first_start(int config_exists, int lodestar_catalog_or_src) {
    return !config_exists && !lodestar_catalog_or_src;
}

long coexist_boot_time(long wall, long mono_sec) {
    if (mono_sec < 0)
        return 0;
    return wall - mono_sec;
}

int coexist_restart_mark_fresh(long mark, long boot) {
    long d = mark - boot;
    if (d < 0)
        d = -d;
    return boot && mark > 0 && d <= 60;
}

int coexist_same_process_reentry_safe(int log_already_running) {
    (void)log_already_running;
    return 0;
}

int coexist_second_load_stops_log(int same_process) {
    (void)same_process;
    return 1;
}

int coexist_migrate_runs(int organize, int already_migrated, int block) {
    (void)block;
    return !organize && !already_migrated;
}

int coexist_rest_lost_lock_stops(int still_listening, int rebind_ok) {
    return !still_listening && !rebind_ok;
}

void coexist_format_json(int installed, int autorun, int state, int block, char *out, size_t n) {
    if (!out || !n)
        return;
    snprintf(out, n,
             "\"coexist\":{\"ps4mount_installed\":%s,\"ps4mount_autorun\":%s,"
             "\"ps4mount_state\":%s,\"block\":%s}",
             installed ? "true" : "false", autorun ? "true" : "false",
             state ? "true" : "false", block ? "true" : "false");
}
