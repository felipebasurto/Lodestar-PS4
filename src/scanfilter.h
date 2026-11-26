/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Filename filter for scan-folder PKG adoption (P4N-21 in specs/ps4-native.md). */
#pragma once

#include <stddef.h>
#include <string.h>
#include <strings.h>

/* 1 when name is a macOS AppleDouble sidecar whose suffix is .pkg (e.g. "._game.pkg"). */
static inline int scan_is_appledouble_pkg(const char *name) {
    if (!name || name[0] != '.' || name[1] != '_')
        return 0;
    size_t n = strlen(name);
    return n >= 6 && !strcasecmp(name + n - 4, ".pkg");
}

/* 1 when name is a candidate PKG filename for scan-folder adoption. */
static inline int scan_pkg_candidate(const char *name) {
    if (!name || name[0] == '.')
        return 0;
    size_t n = strlen(name);
    return n >= 5 && !strcasecmp(name + n - 4, ".pkg");
}
