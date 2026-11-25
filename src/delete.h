/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The pure decisions of the console-delete path. They live here so the host tests can prove
 * the data-loss boundary without the PS4 SDK or a console. */
#pragma once

#include <stdio.h>
#include <string.h>

#include "pkg.h"

/* 1 only when dir is byte-exactly <root>/ps4/<tid> for a valid PS4 title id. Anything else
 * is foreign or in-place user data and must not be removed by the console-delete path. */
static inline int delete_target_ok(const char *root, const char *tid, const char *dir) {
    if (!root || !tid || !dir || !is_game_id(tid))
        return 0;
    char want[128];
    int n = snprintf(want, sizeof(want), "%s/ps4/%s", root, tid);
    return n > 0 && (size_t)n < sizeof(want) && !strcmp(dir, want);
}

/* Screens of the two delete flows: home screen (Options > Delete) and Settings > Storage.
 * Returns 2 for a screen that opens the window, 1 for a screen that keeps it open (menus
 * and dialogs on the way), 0 otherwise. The first entries are inherited PS5 names kept as
 * harmless legacy inputs; the PS4 names are the confirmed production contract. */
static inline int delete_scene_kind(const char *name) {
    static const char *opens[] = {"AppDeleteConfirmScene", "console storage delete content", "DeleteDialogScreen",
                                  "ConfirmContentDeleteScene", "storage_data#application", NULL};
    static const char *keeps[] = {"AppDeletingScene", "ReactPopupMenu", "Noname", "Render.NPXS40021",
                                  "storage management", "ContentDeleteScene", "OptionMenu :", "Parent_NormalStack",
                                  NULL};
    for (int i = 0; opens[i]; i++)
        if (strstr(name, opens[i]))
            return 2;
    for (int i = 0; keeps[i]; i++)
        if (strstr(name, keeps[i]))
            return 1;
    return 0;
}

/* Delete-window transitions. Returns 2 when a scene opens/refreshes the window, 1 when a
 * dialog merely keeps it open, -1 when an unrelated scene closes it, and 0 when nothing
 * changes. kind is delete_scene_kind(). */
static inline int delete_window_transition(int kind, int open) {
    if (kind == 2)
        return 2;
    if (kind == 1 && open)
        return 1;
    if (!kind && open)
        return -1;
    return 0;
}

/* A GameWillDelete line is a user delete only inside the window and only for a title this
 * daemon manages. Outside the window the same line can be the system's own uninstall work
 * (panel removal or registration replacement) and must not delete library files. */
static inline int delete_user_event_ok(int window_open, int managed) { return window_open && managed; }

/* A PlayGo uninstall-end event has the same window rule. 0: ignore, 1: an add-on delete,
 * 2: the base game's delete. Base matches win because their content id is the game's own. */
static inline int delete_playgo_event_kind(int window_open, int base_managed, int dlc_managed) {
    if (!window_open)
        return 0;
    if (base_managed)
        return 2;
    return dlc_managed != 0;
}

/* Panel removal may uninstall only what PS4Mount owns. Internal real installs are refused
 * even when stale state exists; otherwise a managed record, source marker or scanned library
 * location is enough ownership evidence. */
static inline int remove_owned_ok(int installed_internal, int managed, int src_state, int library_root) {
    if (installed_internal)
        return 0;
    return managed || src_state || library_root;
}


/* Removal may reach the system uninstaller only when every target through which the drive
 * content is reachable is detached. */
static inline int remove_detached_ok(int app_mounted, int patch_mounted, int addcont_mounted) {
    return !app_mounted && !patch_mounted && !addcont_mounted;
}

/* The content id in a kernel-log uninstall line is pkg-controlled text before it is a path
 * component. Only the plain charset of content ids may pass; no slash, dot or control byte. */
static inline int delete_content_id_plain(const char *id) {
    if (!id || !*id)
        return 0;
    for (const char *c = id; *c; c++) {
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' ||
              *c == '_'))
            return 0;
    }
    return 1;
}
