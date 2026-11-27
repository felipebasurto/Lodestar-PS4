/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Operational inventory persisted under /data/lodestar before registration
 * operations (specs/registration.md REG-10). Never reads or writes system DBs. */
#pragma once

#include <stdint.h>

#define INV_MAX 768

typedef struct {
    char title_id[10];
    char source[1024];
    char mount_mode[24];
    char game_ver[8];
    char effective_ver[8];
    int has_patch;
    int dlc_count;
    char dlc_mode[16];
    char state[24];
    int64_t updated_at;
} inv_entry_t;

typedef struct {
    inv_entry_t items[INV_MAX];
    int count;
} inv_list_t;

/* Allocates and frees an inventory table on mapped pages (src/mem.h). */
inv_list_t *inv_alloc(void);
void inv_free(inv_list_t *list);

/* Loads a previously saved TSV inventory. Returns number of loaded entries, or -1 if
 * the file cannot be read (missing file leaves list empty and returns 0). */
int inv_load(inv_list_t *list, const char *path);

/* Inserts or updates one title entry by title_id. Returns 0 on success, -1 if full
 * or invalid title_id. */
int inv_upsert(inv_list_t *list, const inv_entry_t *entry);

/* Looks up a title in the loaded inventory. */
const inv_entry_t *inv_find(const inv_list_t *list, const char *title_id);

/* Atomically writes the inventory to path (<path>.tmp -> rename).
 * Refuses any path inside /system_data/priv/mms. Returns 0 on success, -1 on error. */
int inv_save(const inv_list_t *list, const char *path);
