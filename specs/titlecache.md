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

### TCACHE-4: cache fills are serialized because the fill is shared scan evidence
A cache miss may open the bounded pkg set of one title while holding the cache mutex. This is
intentional ownership, not an oversight: the scan owner and panel workers need the same evidence,
so the first consumer that reaches a miss performs the read every consumer would otherwise have
to perform, and later consumers hit the same entry. Moving that read outside the mutex without a
singleflight design would duplicate expensive USB/NAS reads between the scan owner and the two
bounded panel workers. A future singleflight design must preserve invalidation ordering (a hard
scan must not be overwritten by an older in-flight fill), LRU/eviction ownership, add-on pool
lifetime, and load/save boundaries before it can replace this serialization. The mutex boundary
remains limited to one title's bounded pkg set or one cache metadata operation; enumeration,
fingerprinting and response assembly stay outside it.
Check: read for the ownership invariant and its interaction with scan invalidation.
Source: worker cache-miss architecture review 2026-11-25.
