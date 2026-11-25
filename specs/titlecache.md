# Title cache memory spec

Governs: persistent per-drive title metadata in `src/titlecache.c` and `src/titlecache.h`.
Source: rewrite memory audit 2026-11-25.

## Rules

### TCACHE-1: add-on side arrays do not cost one page per game
The cache may keep at most `TITLE_DLC_MAX` small add-on descriptors per game. Those descriptors
are owned by the title cache and are allocated from a bounded internal size-class pool, not as
one `mem_alloc` mapping per game: a game with one add-on must not consume a whole 16 KiB page.
Freed descriptors return to their size class for reuse. Pool blocks may be retained at the
cache lifetime high-water mark; they are never returned through `mem_free` as if they were
single object mappings. The persisted cache format and lookup semantics are unchanged.
Check: test (memory ownership and regression of the page-per-descriptor waste). Test:
`test/titlecache_test.c` › pool scaling case.
Source: rewrite memory audit 2026-11-25.
