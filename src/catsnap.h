/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Immutable grouped-catalog snapshots for panel workers. */
#pragma once

#include <stdint.h>

#define CATSNAP_NO_PATH UINT32_MAX

/* Paths live in one string arena inside the snapshot; offsets are stable for the life of
 * the snapshot. first_dlc/ndlc index the compact add-on array. */
typedef struct {
    char title_id[10];
    uint32_t app_off;
    uint32_t patch_off; /* CATSNAP_NO_PATH without an update */
    uint32_t first_dlc;
    uint32_t ndlc;
} catsnap_game_t;

typedef struct {
    uint32_t path_off;
    char content_id[37];
} catsnap_dlc_t;

typedef struct catsnap catsnap_t;

/* One mapped allocation holding header, games, add-ons and strings. The first reference
 * belongs to the publisher slot; readers add their own with catsnap_ref. */
catsnap_t *catsnap_alloc(uint32_t ngames, uint32_t ndlcs, uint32_t strings_len);
void catsnap_ref(catsnap_t *s);
/* Drops one reference and unmaps the snapshot when the last one goes away. */
void catsnap_unref(catsnap_t *s);

uint32_t catsnap_ngames(const catsnap_t *s);
catsnap_game_t *catsnap_games(catsnap_t *s);
catsnap_dlc_t *catsnap_dlcs(catsnap_t *s);
char *catsnap_strings(catsnap_t *s);
const char *catsnap_path(const catsnap_t *s, uint32_t off);
const catsnap_game_t *catsnap_find(const catsnap_t *s, const char *title_id);
