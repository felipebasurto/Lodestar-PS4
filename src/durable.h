/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Atomic durable replacement for small state and metadata files (SYS-2). */
#pragma once

#include <stddef.h>
#include <sys/types.h>

/* Writes data[0..len] to a unique sibling temp file, sets mode with fchmod, calls fsync,
 * and atomically renames it over path. On any failure before rename succeeds, unlinks the
 * temp file and leaves any existing file at path untouched. Returns 0 on success, -1 on error. */
int durable_replace(const char *path, const void *data, size_t len, mode_t mode);
