/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Emulators: a folder of a drive, <drive>/Lodestar/emulators/<App>/<Folder> (the ROMs, the
 * BIOS), is shown inside the console as /data/<App>/<Folder>, where an emulator app looks
 * for it, so gigabytes of games stay on the drive. No setting: the folder names are the
 * setting. What may be named is decided here, so a test holds it: a name from a drive is
 * input from outside, and the result is a mount over a folder of the console. */
#pragma once

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define EMU_DIR "Lodestar/emulators"

/* A name PS4Mount may use for a folder: letters, digits, space, dot, dash and underscore,
 * 1 to 48 bytes, not starting with a dot (so never "." or ".."), and no slash. */
static inline int emu_name_ok(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 48 || s[0] == '.')
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
                 c == '-' || c == '_';
        if (!ok)
            return 0;
    }
    return 1;
}

/* The folders of /data that are PS4Mount's, the system installer's or a payload's: never the
 * app side of an emulator mount (PS4Mount's own state, its internal library, the pkgs of the
 * installer, GoldHEN and the payloads). */
static inline int emu_app_reserved(const char *app) {
    static const char *const reserved[] = {"ps4mount", "lodestar", "ps4", "pkg", "goldhen", "payloads"};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        if (!strcasecmp(app, reserved[i]))
            return 1;
    return 0;
}

/* Where <App>/<Folder> goes: "/data/<App>/<Folder>" in out. 0 when it may be mounted, -1 when
 * a name is not allowed or the app is reserved. */
static inline int emu_target(const char *app, const char *folder, char *out, size_t n) {
    if (!emu_name_ok(app) || !emu_name_ok(folder) || emu_app_reserved(app))
        return -1;
    return snprintf(out, n, "/data/%s/%s", app, folder) < (int)n ? 0 : -1;
}

/* A path that is a drive's emulators folder, "<drive>/Lodestar/emulators". It is the drive's
 * ROMs and BIOS, not games: the searches for pkgs and the tidying of the scan folder leave it
 * alone (an empty ROM folder would be taken for a game folder left empty). */
static inline int emu_is_dir(const char *path) {
    size_t n = strlen(path), m = strlen("/" EMU_DIR);
    return n >= m && !strcasecmp(path + n - m, "/" EMU_DIR);
}


typedef enum {
    EMU_STEP_REFUSE = 0, /* foreign/unknown state, non-empty target, or no app folder */
    EMU_STEP_ALREADY,    /* the exact source is already the live top layer */
    EMU_STEP_MKDIR_MOUNT,/* target proved absent with ENOENT: create it, then mount */
    EMU_STEP_MOUNT,      /* target exists as an empty real folder */
} emu_step_t;

/* EMU-3: admission uses only observed facts; it never treats an unknown read as an empty or absent
 * target. top is top_layer's contract: 1 exact, 0 no live mount, -1 foreign/other. */
static inline emu_step_t emu_mount_step(int names_ok, int appdir_exists, int top, int target_missing,
                                        int target_stat_error, int target_dir, int target_empty) {
    if (!names_ok || !appdir_exists || top < 0 || target_stat_error)
        return EMU_STEP_REFUSE;
    if (top == 1)
        return EMU_STEP_ALREADY;
    if (target_missing)
        return EMU_STEP_MKDIR_MOUNT;
    return target_dir && target_empty ? EMU_STEP_MOUNT : EMU_STEP_REFUSE;
}

/* EMU-4 ownership: from must be exactly <root>/Lodestar/emulators/<App>/<Folder> and on exactly
 * /data/<App>/<Folder> for the same pair. A prefix lookalike is not PS4Mount's layer. */
static inline int emu_mount_owned(const char *root, const char *from, const char *on) {
    if (!root || !from || !on || strncmp(on, "/data/", 6))
        return 0;
    static const char prefix[] = "/" EMU_DIR "/";
    size_t rl = strlen(root), pl = sizeof(prefix) - 1;
    return !strncmp(from, root, rl) && !strncmp(from + rl, prefix, pl) && !strcmp(from + rl + pl, on + 6);
}

/* EMU-4: a stale owned layer may be removed only when it is the single layer on its target and the
 * source is gone or the layer is dead. If the mount table cannot prove that single-layer fact, keep it. */
static inline int emu_stale_unmount_ok(int source_present, int dead, int layers) {
    return (!source_present || dead) && layers == 1;
}
