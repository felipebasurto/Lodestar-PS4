/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * A bounded, scan-scoped snapshot of PS4 BGFT game-task metadata. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bgftinfo.h"

typedef struct {
    uint32_t id;
    uint32_t error;
    int malformed;
    bgft_info_t info;
} bgftscan_entry_t;

typedef struct {
    bgftscan_entry_t *entries;
    size_t count, capacity;
    uint8_t *used; /* bitset of task directories, including unidentifiable and install ids */
    uint32_t used_first;
    size_t used_count;
    int usable; /* 0 means the snapshot could not be built; callers must use fresh reads */
} bgftscan_t;

void bgftscan_init(bgftscan_t *s);
void bgftscan_free(bgftscan_t *s);
/* Reads every numeric task directory in [first,last] once. A missing directory is a valid
 * empty snapshot; allocation failure is not and leaves usable false. Unreadable tasks are
 * absent. Malformed tasks with enough evidence to identify their title are present but never
 * count as healthy. */
void bgftscan_load(bgftscan_t *s, const char *dir, uint32_t first, uint32_t last);
/* Updates the epoch from bytes just written by the caller. 0 leaves/keeps it unusable. */
int bgftscan_update(bgftscan_t *s, uint32_t id, const uint8_t *data, size_t len);
void bgftscan_remove_id(bgftscan_t *s, uint32_t id);
/* Whether the numeric task directory id existed in the epoch, even when its PDB could not
 * be parsed. IDs outside the snapshotted range answer 0. */
int bgftscan_id_used(const bgftscan_t *s, uint32_t id);
/* Boot-scoped task ids known to be occupied even when their directories do not exist yet. */
typedef struct {
    uint8_t bits[0x10000 / 8];
} bgft_taken_t;

void bgft_taken_add(bgft_taken_t *taken, uint32_t id);
int bgft_taken_has(const bgft_taken_t *taken, uint32_t id);
/* Allocates a new odd game-task id whose even install-task id is free too. Occupancy is the
 * scan epoch when usable (or direct directory evidence otherwise) plus the boot-scoped taken
 * set. 0 means occupancy could not be proven or no id is free. */
uint32_t bgftscan_alloc_id(const bgftscan_t *s, const bgft_taken_t *taken, const char *dir,
                           uint32_t first, uint32_t last);
const bgftscan_entry_t *bgftscan_find(const bgftscan_t *s, const char *tid);
/* 1 when a well-formed task's canonical URL record is exactly url. */
int bgftscan_names(const bgftscan_entry_t *e, const char *url);
