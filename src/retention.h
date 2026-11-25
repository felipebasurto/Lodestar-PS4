/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Pure parsing of PS4Mount's .removed retention markers. The filenames are state written
 * by the daemon but read back as external input after crashes, edits and mixed versions. */
#pragma once

#include <string.h>

#include "delete.h"
#include "pkg.h"

typedef enum {
    RETENTION_MARK_NONE = 0,
    RETENTION_MARK_BASE,
    RETENTION_MARK_DLC,
} retention_mark_t;

#define RETENTION_SUFFIX ".removed"
#define RETENTION_SUFFIX_LEN 8

/* Classifies exactly <TID>.removed or <CONTENT_ID>.removed and returns the game title id
 * the marker belongs to. A DLC content id carries it at bytes 7..15. Anything malformed is
 * no retention evidence at all; callers must not use partial bytes from it. */
static inline retention_mark_t retention_marker(const char *name, char tid[10]) {
    if (!name)
        return RETENTION_MARK_NONE;
    size_t len = strlen(name);
    if (len == 9 + RETENTION_SUFFIX_LEN && !strcmp(name + 9, RETENTION_SUFFIX)) {
        char id[10];
        memcpy(id, name, 9);
        id[9] = 0;
        if (!is_game_id(id))
            return RETENTION_MARK_NONE;
        if (tid)
            memcpy(tid, id, 10);
        return RETENTION_MARK_BASE;
    }
    if (len == 36 + RETENTION_SUFFIX_LEN && !strcmp(name + 36, RETENTION_SUFFIX)) {
        char cid[37], id[10];
        memcpy(cid, name, 36);
        cid[36] = 0;
        if (!delete_content_id_plain(cid))
            return RETENTION_MARK_NONE;
        memcpy(id, cid + 7, 9);
        id[9] = 0;
        if (!is_game_id(id))
            return RETENTION_MARK_NONE;
        if (tid)
            memcpy(tid, id, 10);
        return RETENTION_MARK_DLC;
    }
    return RETENTION_MARK_NONE;
}


/* Complete deletion decision for one DLC marker in one title-scoped retention event. The
 * unlink caller still owns the filesystem effect; this pure boundary decides whether the
 * marker belongs to the title the user action names. */
static inline int retention_clear_dlc_for_tid(const char *name, const char *tid) {
    char mtid[10];
    return tid && retention_marker(name, mtid) == RETENTION_MARK_DLC && !strcmp(mtid, tid);
}
