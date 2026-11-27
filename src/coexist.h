/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Read-only PS4Mount coexistence probe and automatic-registration guard (P4N-32, REG-12). */
#pragma once

#include <stddef.h>

typedef struct {
    const char *goldhen_payloads;
    const char *payloads;
    const char *payloads_ini;
    const char *user_app;
    const char *ps4mount_state_dir;
    const char *ps4mount_sys_dir;
} coexist_paths_t;

typedef struct {
    int ps4mount_payload;
    int ps4mount_app;
    int ps4mount_shortcut;
    int ps4mount_autorun;
    int ps4mount_state;
} coexist_state_t;

coexist_paths_t coexist_console_paths(void);
void coexist_probe(const coexist_paths_t *paths, coexist_state_t *out);

int coexist_ps4mount_installed(const coexist_state_t *s);
int coexist_block_auto_register(const coexist_state_t *s);

/* REG-12: while block is active, only an explicitly requested manual Add title may register. */
int coexist_title_register_allowed(int block, const char *tid, const char *manual_tid);

/* P4N-32: migrate_organized does not move <drive>/ps4/<TID> while PS4Mount is present. */
int coexist_migrate_allowed(int organize, int db_corrupt, int coexist_block);

/* Writes the JSON object for the "coexist" field in /api/status and /api/games.
 * Returns number of bytes written (excluding NUL), or 0 on error. */
size_t coexist_format_json(const coexist_state_t *s, char *out, size_t cap);
