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
