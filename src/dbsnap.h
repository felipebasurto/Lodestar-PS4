/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * When a successful app.db read may replace the catalog snapshot. SYS-1. */
#pragma once

/* 1 when this successful read may become the trusted title list.
 * have_trusted/trusted_n describe the last published snapshot.
 * journal_present is 1 when app.db-journal exists at the moment of the decision.
 * The first read of a run is not published while the journal exists: there is no
 * earlier snapshot to keep, and a hot journal means a writer is mid-update.
 * A later read that lists fewer titles is not published while the journal exists,
 * so a torn snapshot cannot prove that a title disappeared. Growth and an equal
 * count are published. A shrink after the journal is gone is published, including
 * an empty catalog after a real rebuild. */
static inline int db_snapshot_publish(int have_trusted, int trusted_n, int new_n, int journal_present) {
    if (new_n < 0)
        return 0;
    if (!journal_present)
        return 1;
    if (!have_trusted || new_n < trusted_n)
        return 0;
    return 1;
}
