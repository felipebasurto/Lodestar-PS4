/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Pure restore admission and path decisions. Restore handles user packages and state written on
 * writable media, so these boundaries are decided here and only executed in main.c. */
#pragma once

#include <string.h>

/* REST-2/REST-3: per-title state may be cleared only after that title crossed the system-removal
 * commit boundary and its file moves did not fail. A failed unregister is not cleanup evidence, and
 * a failed move must leave the suppression marker in place rather than letting a scan re-register
 * the leftovers as if restore had never been requested. */
static inline int restore_clear_state_ok(int unregister_ok, int move_failed) {
    return unregister_ok != 0 && move_failed == 0;
}

/* REST-4: orig comes from origin.txt (media anyone can write) or package metadata. Joined under
 * the drive root it must be a normal relative file path: no absolute path, backslash, empty
 * component, "." component or ".." component. */
static inline int restore_orig_path_ok(const char *o) {
    if (!o || !o[0] || o[0] == '/' || strchr(o, '\\'))
        return 0;
    for (const char *p = o; *p;) {
        const char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (!l || (l == 1 && p[0] == '.') || (l == 2 && p[0] == '.' && p[1] == '.'))
            return 0;
        if (!e)
            break;
        p = e + 1;
        if (!*p) /* trailing slash: a directory, not a package path */
            return 0;
    }
    return 1;
}
