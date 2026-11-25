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

### TCACHE-2: registration markers are stored without duplicating the game directory
The cache may keep the registration marker's decision suffix (update/add-on digests), but not a
second copy of the game directory already stored in `dir`. The external `.src` marker format and
the persisted cache format remain the full historical text: `<dir>\n<patch-or-nopatch>\n` plus
the optional add-on digest line. Loading accepts that historical full form; saving writes it back.
A cached entry whose marker's first line is not exactly its directory is damaged and ignored.
Check: test (external cache format and memory representation). Test: `test/titlecache_test.c` ›
marker round-trip case.
Source: rewrite memory audit 2026-11-25.

### TCACHE-3: cache lookup is indexed without making the index authoritative
Per-drive title lookup must not walk every cached entry for every scanned game on the hit path.
A private open-addressed root/title index may accelerate lookup, but every hit must still verify
root, title id and file fingerprint. Stale slots after LRU replacement are harmless probes, never
ownership evidence. A miss may fall back to the exact linear scan, and forgetting or rebuilding
the cache removes stale index state.
Check: test (large input space and lookup behavior). Test: `test/titlecache_test.c` existing
cache lookup/eviction cases.
Source: scan algorithmic audit 2026-11-25.
