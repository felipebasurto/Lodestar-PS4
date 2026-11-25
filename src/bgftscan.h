/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * A bounded, scan-scoped snapshot of PS4 BGFT game-task metadata. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "shortpath.h"

typedef struct {
    uint32_t id;
    uint32_t error;
    uint8_t url_len;
    uint8_t malformed;
    char tid[10];
    char url[SYS_PATH_MAX + 1];
} bgftscan_entry_t;

typedef struct {
    bgftscan_entry_t *entries;
    size_t count;
    int usable; /* 0 means the snapshot could not be built; callers must use fresh reads */
} bgftscan_t;

void bgftscan_init(bgftscan_t *s);
void bgftscan_free(bgftscan_t *s);
/* Loads every numeric task directory in [first,last] once. A missing directory is a valid
 * empty snapshot; allocation failure is not and leaves usable false. */
void bgftscan_load(bgftscan_t *s, const char *dir, uint32_t first, uint32_t last);
const bgftscan_entry_t *bgftscan_find(const bgftscan_t *s, const char *tid);
/* 1 when the task's canonical URL record names url. Malformed tasks never match. */
int bgftscan_names(const bgftscan_entry_t *e, const char *url);
