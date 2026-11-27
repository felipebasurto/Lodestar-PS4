/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Replace one file so a crash cannot leave a short prefix. SYS-2. */
#pragma once

#include <stddef.h>
#include <sys/types.h>

/* Writes data to a temporary file in the same directory and renames it over path.
 * The destination is never opened with O_TRUNC. 0 on success, -1 on failure
 * (the previous directory entry is unchanged). A symlink at path is replaced,
 * not followed. An existing regular file keeps its permission bits; a new file
 * uses mode exactly (the process umask is not applied).
 *
 * durable_fail_after_temp is a host-test seam. The payload leaves it NULL.
 * When it is set and returns non-zero, the temporary file is removed and path
 * is left unchanged. */
int durable_replace(const char *path, const void *data, size_t len, mode_t mode);

extern int (*durable_fail_after_temp)(void);
