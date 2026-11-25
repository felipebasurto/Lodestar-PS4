# Catalog memory spec

Governs: drive package catalogs in `src/catalog.c` and `src/catalog.h`.
Sources: rewrite memory audit 2026-11-25 and the production call `catalog_link(..., cross=0)`.

## Rules

### CAT-1: cross-drive references are optional side storage
Local grouping data is part of every `cat_game_t`; cross-drive update/add-on references are
not. The daemon's production mode links catalogs with `cross=0`, so it must not carry the
fixed cross-drive arrays in every game. When `catalog_link(..., cross=1)` is used, bounded
side storage is allocated for the catalogs participating in that link and released by
unlinking, regrouping, or freeing the catalog.
Check: test (memory ownership and public catalog behavior). Test: `test/catalog_test.c` ›
`test_choose`; the cross-drive case in that test must keep passing.
Source: rewrite memory audit 2026-11-25.

### CAT-2: cross-link failure degrades without inventing ownership
If optional cross storage cannot be allocated, that catalog simply has no cross-drive
references. Local grouping remains valid; missing memory must not be interpreted as proof
that an update or add-on is absent everywhere.
Check: read (allocation-failure injection is not available in the catalog host harness).
Source: rewrite memory audit 2026-11-25.

### CAT-3: title lookup is indexed for large catalogs
Grouping and public title lookup must not walk every game for every package. A catalog may keep
a private open-addressed index from title id to game index, rebuilt whenever games are regrouped.
The index is only an accelerator: package roles, duplicate handling, cross references, and lookup
results are identical to the linear scan. If the index cannot be allocated, grouping and lookup
fall back to the linear scan. Walks that invalidate game indexes do not reuse stale entries.
Check: test (large input space and public catalog behavior). Test: `test/catalog_test.c` ›
large catalog lookup case.
Source: scan algorithmic audit 2026-11-25.

### CAT-4: add-on names and service lists share category-specific storage
A package is either an add-on (`ac`) or a game/update. Add-on display names are meaningful only
for `ac`; `SERVICE_ID_ADDCONT_ADD_n` lists are meaningful only when grouping a game or update.
The catalog may therefore store them in category-specific overlapping storage. The persisted
cache format remains unchanged. Service lists found on add-ons and display names found on
games/updates are meaningless input and are not preserved as grouping evidence.
Check: test (external persisted catalog format and grouping behavior). Tests:
`test/catalog_test.c` existing service/add-on grouping cases.
Source: catalog memory audit 2026-11-25.
