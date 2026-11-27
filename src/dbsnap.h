/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Pure snapshot publication policy for read-only nolock=1 reads of app.db (SYS-1). */
#pragma once

/* Returns 1 when a freshly read app.db title list may replace the cached snapshot.
 * Rejects:
 *  - any failed query (!read_ok),
 *  - an empty catalog (new_count <= 0: a live PS4 app.db always contains system titles;
 *    an empty-but-successful read after a rebuild must not mark db_ready() or confirm
 *    titles absent),
 *  - a first read or shrunk list while app.db-journal or app.db-wal is present on disk. */
static inline int db_snapshot_publish(int read_ok, int has_prior, int prior_count, int new_count,
                                      int journal_active) {
    if (!read_ok || new_count <= 0)
        return 0;
    if (journal_active && (!has_prior || new_count < prior_count))
        return 0;
    return 1;
}
