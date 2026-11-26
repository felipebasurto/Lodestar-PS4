/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Immutable grouped-catalog snapshots for panel workers. */
#include "catsnap.h"

#include <stddef.h>
#include <string.h>

#include "mem.h"

struct catsnap {
    uint32_t refs;
    uint32_t ngames, ndlcs, strings_len;
    unsigned char data[];
};

static size_t aligned(size_t n) { return (n + 7u) & ~7u; }

static size_t games_off(void) { return aligned(sizeof(catsnap_t)); }

static size_t dlcs_off(uint32_t ngames) { return games_off() + (size_t)ngames * sizeof(catsnap_game_t); }

static size_t strings_off(uint32_t ngames, uint32_t ndlcs) {
    return aligned(dlcs_off(ngames) + (size_t)ndlcs * sizeof(catsnap_dlc_t));
}

catsnap_t *catsnap_alloc(uint32_t ngames, uint32_t ndlcs, uint32_t strings_len) {
    size_t end = strings_off(ngames, ndlcs);
    if ((size_t)ngames > (SIZE_MAX - end) / sizeof(catsnap_game_t) ||
        (size_t)ndlcs > (SIZE_MAX - end) / sizeof(catsnap_dlc_t) || end > SIZE_MAX - strings_len)
        return NULL;
    size_t total = end + strings_len;
    catsnap_t *s = mem_alloc(total);
    if (!s)
        return NULL;
    s->refs = 1;
    s->ngames = ngames;
    s->ndlcs = ndlcs;
    s->strings_len = strings_len;
    return s;
}

void catsnap_ref(catsnap_t *s) {
    if (s)
        __atomic_add_fetch(&s->refs, 1, __ATOMIC_ACQ_REL);
}

void catsnap_unref(catsnap_t *s) {
    if (s && __atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL) == 0)
        mem_free(s);
}

uint32_t catsnap_ngames(const catsnap_t *s) { return s ? s->ngames : 0; }

catsnap_game_t *catsnap_games(catsnap_t *s) { return (catsnap_game_t *)((unsigned char *)s + games_off()); }

catsnap_dlc_t *catsnap_dlcs(catsnap_t *s) { return (catsnap_dlc_t *)((unsigned char *)s + dlcs_off(s->ngames)); }

char *catsnap_strings(catsnap_t *s) { return (char *)s + strings_off(s->ngames, s->ndlcs); }

const char *catsnap_path(const catsnap_t *s, uint32_t off) {
    if (!s || off == CATSNAP_NO_PATH || off >= s->strings_len)
        return "";
    return (const char *)s + strings_off(s->ngames, s->ndlcs) + off;
}

const catsnap_game_t *catsnap_find(const catsnap_t *s, const char *title_id) {
    if (!s)
        return NULL;
    catsnap_game_t *games = catsnap_games((catsnap_t *)s);
    for (uint32_t i = 0; i < s->ngames; i++)
        if (!strcmp(games[i].title_id, title_id))
            return &games[i];
    return NULL;
}
