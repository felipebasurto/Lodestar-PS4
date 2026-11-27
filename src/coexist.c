/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Read-only PS4Mount coexistence probe and automatic-registration guard (P4N-32, REG-12).
 * Never creates, modifies, or deletes any PS4Mount or system file. */
#include "coexist.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MIN_PAYLOAD_SIZE 4096

coexist_paths_t coexist_console_paths(void) {
    coexist_paths_t p;
    p.goldhen_payloads = "/data/GoldHEN/payloads";
    p.payloads = "/data/payloads";
    p.payloads_ini = "/data/GoldHEN/payloads.ini";
    p.user_app = "/user/app";
    p.ps4mount_state_dir = "/data/ps4mount";
    p.ps4mount_sys_dir = "/system_data/ps4mount";
    return p;
}

static int dir_exists(const char *path) {
    struct stat st;
    return path && *path && !stat(path, &st) && S_ISDIR(st.st_mode);
}

static int subdir_exists(const char *dir, const char *name) {
    if (!dir || !*dir || !name || !*name)
        return 0;
    char p[PATH_MAX];
    int n = snprintf(p, sizeof(p), "%s/%s", dir, name);
    return n > 0 && (size_t)n < sizeof(p) && dir_exists(p);
}

/* Matches real ELF (\x7fELF) or raw binboot/x86-64 payload (starts with UH / 0x55 0x48 or ELF). */
static int payload_file_present(const char *dir, const char *name) {
    if (!dir || !*dir || !name || !*name)
        return 0;
    char p[PATH_MAX];
    int n = snprintf(p, sizeof(p), "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof(p))
        return 0;
    struct stat st;
    if (stat(p, &st) || !S_ISREG(st.st_mode) || st.st_size < MIN_PAYLOAD_SIZE)
        return 0;
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return 0;
    uint8_t magic[4] = {0};
    ssize_t r = read(fd, magic, sizeof(magic));
    close(fd);
    if (r < 4)
        return 0;
    int is_elf = magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
    int is_bin = magic[0] == 0x55 && magic[1] == 0x48;
    return is_elf || is_bin;
}

static int eq_ci(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    if (alen != blen)
        return 0;
    for (size_t i = 0; i < alen; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

/* 1 when line is an active (non-comment, non-zero) GoldHEN payloads.ini entry for ps4mount-ps4. */
static int line_is_ps4mount_autorun(const char *line, size_t len) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= len || line[i] == '#' || line[i] == ';' || line[i] == '[')
        return 0;
    const char *eq = memchr(line + i, '=', len - i);
    size_t key_end = eq ? (size_t)(eq - line) : len;
    if (eq) {
        size_t v = (size_t)(eq - line) + 1;
        while (v < len && (line[v] == ' ' || line[v] == '\t'))
            v++;
        if (v < len && line[v] == '0')
            return 0;
    }
    while (key_end > i && (line[key_end - 1] == ' ' || line[key_end - 1] == '\t' || line[key_end - 1] == '\r'))
        key_end--;
    size_t base = i;
    for (size_t k = i; k < key_end; k++)
        if (line[k] == '/' || line[k] == '\\')
            base = k + 1;
    size_t blen = key_end - base;
    return eq_ci(line + base, blen, "ps4mount-ps4.bin") || eq_ci(line + base, blen, "ps4mount-ps4.elf");
}

static int ini_has_ps4mount_autorun(const char *ini_path) {
    if (!ini_path || !*ini_path)
        return 0;
    int fd = open(ini_path, O_RDONLY);
    if (fd < 0)
        return 0;
    char buf[16384];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    int in_autorun = 0;
    const char *p = buf;
    const char *end = buf + n;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        size_t s = 0;
        while (s < len && (p[s] == ' ' || p[s] == '\t'))
            s++;
        if (s < len && p[s] == '[') {
            const char *rb = memchr(p + s, ']', len - s);
            in_autorun = rb && eq_ci(p + s + 1, (size_t)(rb - (p + s + 1)), "AutoRun");
        } else if (in_autorun && line_is_ps4mount_autorun(p, len)) {
            return 1;
        }
        p = nl ? nl + 1 : end;
    }
    return 0;
}

void coexist_probe(const coexist_paths_t *paths, coexist_state_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    coexist_paths_t p = paths ? *paths : coexist_console_paths();
    out->ps4mount_payload = payload_file_present(p.goldhen_payloads, "ps4mount-ps4.bin") ||
                            payload_file_present(p.goldhen_payloads, "ps4mount-ps4.elf") ||
                            payload_file_present(p.payloads, "ps4mount-ps4.bin") ||
                            payload_file_present(p.payloads, "ps4mount-ps4.elf");
    out->ps4mount_app = subdir_exists(p.user_app, "PSMT00001");
    out->ps4mount_shortcut = subdir_exists(p.user_app, "PMNT00001");
    out->ps4mount_autorun = ini_has_ps4mount_autorun(p.payloads_ini);
    out->ps4mount_state = dir_exists(p.ps4mount_state_dir) || dir_exists(p.ps4mount_sys_dir);
}

int coexist_ps4mount_installed(const coexist_state_t *s) {
    return s && (s->ps4mount_payload || s->ps4mount_app || s->ps4mount_shortcut);
}

int coexist_block_auto_register(const coexist_state_t *s) {
    return s && (coexist_ps4mount_installed(s) || s->ps4mount_autorun);
}

int coexist_title_register_allowed(int block, const char *tid, const char *manual_tid) {
    if (!block)
        return 1;
    return tid && *tid && manual_tid && *manual_tid && !strcmp(tid, manual_tid);
}

int coexist_migrate_allowed(int organize, int db_corrupt, int coexist_block) {
    return !organize && !db_corrupt && !coexist_block;
}

size_t coexist_format_json(const coexist_state_t *s, char *out, size_t cap) {
    if (!out || !cap)
        return 0;
    coexist_state_t z;
    memset(&z, 0, sizeof(z));
    if (!s)
        s = &z;
    int installed = coexist_ps4mount_installed(s);
    int blocked = coexist_block_auto_register(s);
    const char *reasons[4];
    int nr = 0;
    if (s->ps4mount_payload)
        reasons[nr++] = "payload";
    if (s->ps4mount_app)
        reasons[nr++] = "PSMT00001";
    if (s->ps4mount_shortcut)
        reasons[nr++] = "PMNT00001";
    if (s->ps4mount_autorun)
        reasons[nr++] = "autorun";
    char rbuf[128] = "";
    size_t rpos = 0;
    for (int i = 0; i < nr && rpos + 16 < sizeof(rbuf); i++) {
        int w = snprintf(rbuf + rpos, sizeof(rbuf) - rpos, "%s\"%s\"", i ? "," : "", reasons[i]);
        if (w > 0 && rpos + (size_t)w < sizeof(rbuf))
            rpos += (size_t)w;
    }
    int n = snprintf(out, cap,
                     "{\"ps4mount_installed\":%s,\"ps4mount_autorun\":%s,\"ps4mount_state\":%s,"
                     "\"auto_register_blocked\":%s,\"reasons\":[%s]}",
                     installed ? "true" : "false", s->ps4mount_autorun ? "true" : "false",
                     s->ps4mount_state ? "true" : "false", blocked ? "true" : "false", rbuf);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}
