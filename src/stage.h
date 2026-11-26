/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * What to do with a file in the folder where pkgs sent from the panel wait for the
 * system's installer (INSTALL_DIR). This is the only place that decides when one of
 * PS4Mount's own copies may be deleted: the timings live here so a test can hold them. */
#pragma once

#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>

#include "check.h"
#include "pkg.h"

/* A pkg is looked at for deletion once it is this old (seconds): the system has had time
 * to take it, and the checks in install_cleanup (registered, whole app.pkg of the same
 * size) say whether it did. */
#define INSTALL_KEEP_SECONDS 600
/* A pkg that is still there after this long was never installed (refused, cancelled,
 * paused for good). A paused install of a big game can wait days: this is long. */
#define INSTALL_ABANDON_SECONDS (3 * 86400)
/* The unfinished copy of a pkg being received: deleted only when no transfer is running. */
#define INSTALL_PART_STALE_SECONDS 7200
#define UPLOAD_PART_NAME ".ps4mount-upload.part"
#define PANEL_PART_NAME "lodestar-panel.part"
#define LEGACY_PANEL_PART_NAME "ps4mount-panel.part"

typedef enum {
    STAGE_KEEP = 0,       /* leave it: not ours, or too young */
    STAGE_DELETE_PART,    /* an unfinished transfer nobody is writing */
    STAGE_DELETE_ABANDONED, /* a whole pkg the system never installed */
    STAGE_VERIFY,         /* old enough: delete only if the system has the whole app */
} stage_action_t;


/* STORE-3: the system's installed-file evidence is kind-specific. The add-on content id must be
 * full-length and embed the same game title id as the package; otherwise no path is produced. */
static inline int stage_install_paths(int kind, const char *tid, const char *content_id, char *root, size_t rn,
                                      char *target, size_t tn) {
    if (!root || !target || !is_game_id(tid))
        return -1;
    int rr = -1, tr = -1;
    if (kind == PKG_KIND_GAME) {
        rr = snprintf(root, rn, "/user/app/%s", tid);
        tr = snprintf(target, tn, "%s/app.pkg", root);
    } else if (kind == PKG_KIND_UPDATE) {
        rr = snprintf(root, rn, "/user/patch/%s", tid);
        tr = snprintf(target, tn, "%s/patch.pkg", root);
    } else if (kind == PKG_KIND_ADDON) {
        if (!content_id || strlen(content_id) != 36 || strncmp(content_id + 7, tid, 9))
            return -1;
        for (const char *c = content_id; *c; c++)
            if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' ||
                  *c == '_'))
                return -1;
        rr = snprintf(root, rn, "/user/addcont/%s", tid);
        tr = snprintf(target, tn, "%s/%s/ac.pkg", root, content_id + 20);
    } else {
        return -1;
    }
    return rr < 0 || tr < 0 || (size_t)rr >= rn || (size_t)tr >= tn ? -1 : 0;
}

/* STORE-3: positive evidence that the system has the whole staged app. This is deliberately
 * a conjunction of independent facts; “registered sometime in a cached database snapshot” or
 * “some object exists at the target” is not enough to delete PS4Mount's copy early. */
static inline int stage_installed_ok(int tid_valid, int target_unmounted, int db_confirmed_registered,
                                     int target_regular, uint64_t target_size, uint64_t copy_size) {
    return tid_valid && target_unmounted && db_confirmed_registered && target_regular && copy_size &&
           target_size == copy_size;
}

/* name: the file's name in INSTALL_DIR; age: seconds since it was last written;
 * receiving: 1 while a pkg is being received from the panel. */
static inline stage_action_t stage_action(const char *name, long age, int receiving) {
    size_t l = strlen(name);
    if (!strcmp(name, UPLOAD_PART_NAME) || !strcmp(name, PANEL_PART_NAME) || !strcmp(name, LEGACY_PANEL_PART_NAME))
        return !receiving && age >= INSTALL_PART_STALE_SECONDS ? STAGE_DELETE_PART : STAGE_KEEP;
    if (l < 5 || strcmp(name + l - 4, ".pkg"))
        return STAGE_KEEP; /* anything else is not ours to delete */
    if (age >= INSTALL_ABANDON_SECONDS)
        return STAGE_DELETE_ABANDONED;
    return age >= INSTALL_KEEP_SECONDS ? STAGE_VERIFY : STAGE_KEEP;
}
