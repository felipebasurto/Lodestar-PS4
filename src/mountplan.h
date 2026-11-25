/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Pure mount ownership decisions. They live here so the host tests can prove the boundary
 * without the PS4 SDK, a kernel mount table or a console. */
#pragma once

#include <stdio.h>
#include <string.h>

#include "pkg.h"

typedef enum {
    MOUNT_OWNER_FOREIGN = 0,
    MOUNT_OWNER_EXACT,
    MOUNT_OWNER_LEGACY,
} mount_owner_t;

typedef enum {
    MOUNT_TARGET_NONE = 0,
    MOUNT_TARGET_APP,
    MOUNT_TARGET_PATCH,
    MOUNT_TARGET_ADDCONT,
} mount_target_kind_t;

/* Parses exactly /user/app/<TID>, /user/patch/<TID> or /user/addcont/<TID>. No prefix or
 * suffix is accepted: ownership decisions need the whole target, not a path underneath it. */
static inline mount_target_kind_t mount_target_kind(const char *target, char tid[10]) {
    static const struct {
        const char *prefix;
        mount_target_kind_t kind;
    } kinds[] = {
        {"/user/app/", MOUNT_TARGET_APP},
        {"/user/patch/", MOUNT_TARGET_PATCH},
        {"/user/addcont/", MOUNT_TARGET_ADDCONT},
    };
    if (!target)
        return MOUNT_TARGET_NONE;
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        size_t n = strlen(kinds[i].prefix);
        if (strncmp(target, kinds[i].prefix, n) || strlen(target) != n + 9)
            continue;
        char id[10];
        memcpy(id, target + n, 9);
        id[9] = 0;
        if (!is_game_id(id))
            return MOUNT_TARGET_NONE;
        if (tid)
            memcpy(tid, id, 10);
        return kinds[i].kind;
    }
    return MOUNT_TARGET_NONE;
}

/* Ownership of one observed layer. expected is the exact hold source this daemon intends.
 * marker_dir is the first line of STATE_DIR/<TID>.src when the target is app/patch and it
 * could be read; NULL otherwise. Only exact hold sources and proven legacy direct /mnt
 * sources are owned. Everything else is foreign, even when it looks plausible. */
static inline mount_owner_t mount_layer_owner(const char *target, const char *expected, const char *fstype,
                                              const char *from, const char *marker_dir) {
    if (!target || !expected || !fstype || !from || strcmp(fstype, "nullfs"))
        return MOUNT_OWNER_FOREIGN;
    char tid[10];
    mount_target_kind_t kind = mount_target_kind(target, tid);
    if (kind == MOUNT_TARGET_NONE)
        return MOUNT_OWNER_FOREIGN;
    if (!strcmp(from, expected))
        return MOUNT_OWNER_EXACT;
    if ((kind == MOUNT_TARGET_APP || kind == MOUNT_TARGET_PATCH) && marker_dir && !strncmp(from, "/mnt/", 5) &&
        !strcmp(from, marker_dir))
        return MOUNT_OWNER_LEGACY;
    return MOUNT_OWNER_FOREIGN;
}

static inline int mount_owner_owned(mount_owner_t owner) {
    return owner == MOUNT_OWNER_EXACT || owner == MOUNT_OWNER_LEGACY;
}

typedef enum {
    MOUNT_STEP_REFUSE = -2,
    MOUNT_STEP_UNKNOWN = -1,
    MOUNT_STEP_DONE = 0,
    MOUNT_STEP_UNMOUNT = 1,
} mount_step_t;

/* One detach step from observed facts. top means statfs saw a live mount exactly on the
 * target; when it did not, the only safe way to remove an invisible top is that every layer
 * in the table is owned. */
static inline mount_step_t mount_detach_step(int table_known, int layers, int top, int top_owned, int all_owned) {
    if (!table_known)
        return MOUNT_STEP_UNKNOWN;
    if (top && !layers)
        return MOUNT_STEP_UNKNOWN; /* statfs and the table disagree: no safe mutation */
    if (!layers)
        return MOUNT_STEP_DONE;
    if (top)
        return top_owned ? MOUNT_STEP_UNMOUNT : MOUNT_STEP_REFUSE;
    return all_owned ? MOUNT_STEP_UNMOUNT : MOUNT_STEP_REFUSE;
}

typedef enum {
    MOUNT_PREPARE_REFUSE = -2,
    MOUNT_PREPARE_UNKNOWN = -1,
    MOUNT_PREPARE_MOUNT = 0,
    MOUNT_PREPARE_UNMOUNT = 1,
    MOUNT_PREPARE_ALREADY = 2,
} mount_prepare_t;

/* One routine-mount step. An exact top is already the postcondition even when older owned
 * layers remain underneath: they are not what the system sees and need not be disturbed. */
static inline mount_prepare_t mount_prepare_step(int table_known, int layers, int top, int top_exact, int top_owned,
                                                 int all_owned) {
    if (!table_known)
        return MOUNT_PREPARE_UNKNOWN;
    if (top && !layers)
        return MOUNT_PREPARE_UNKNOWN; /* statfs and the table disagree: no safe mutation */
    if (top_exact)
        return MOUNT_PREPARE_ALREADY;
    if (!layers)
        return MOUNT_PREPARE_MOUNT;
    if (top) {
        if (!top_owned)
            return MOUNT_PREPARE_REFUSE;
        /* A foreign layer underneath makes the replacement impossible. Refuse without
         * destroying the owned state that is currently recoverable. */
        return all_owned ? MOUNT_PREPARE_UNMOUNT : MOUNT_PREPARE_REFUSE;
    }
    return all_owned ? MOUNT_PREPARE_UNMOUNT : MOUNT_PREPARE_REFUSE;
}
