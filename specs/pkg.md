# PKG and param.sfo spec

Governs: `src/pkg.c` and `src/pkg.h`, with consumers in `src/check.c`, `src/main.c`,
`src/catalog.c` and `src/upload.c`.
Sources: psdevwiki PKG/Param.sfo notes, the user's Toshiba library (read-only measurement on
2026-11-25: 89 readable pkgs, 11 to 54 PKG entries, 9 to 44 param.sfo entries), and the PS4-native
memory constraints in `specs/ps4-native.md`.

## Rules

### PKG-1: the PKG entry cap is a validity bound, not a memory layout
A pkg may have at most `PKG_MAX_ENTRIES` entries. The reader stores the entries the file actually
has, not a fixed worst-case table for every open pkg. Each retained entry is the raw 32-byte table
record needed for lookup and decryption; parsed copies of fields already present in that record are
not duplicated. Rejecting a pkg above the cap is unchanged; memory use must scale with the real entry
count.
Check: test (external file format and large input space). Test: `test/unit.c` › `test_pkg_reader`.
Source: rewrite memory audit 2026-11-25.

### PKG-2: param.sfo keeps decision metadata, not unbounded unknown strings
`param.sfo` is external input. The parser validates every key/data offset it accepts and keeps at most
128 entries (the historical cap). Keys are kept to 31 bytes and string values to 95 bytes, NUL-terminated;
integers are exposed as little-endian u32. Those bounds cover every key and value PS4Mount consumes and every
real title/value measured in the Toshiba library (longest key 24 bytes, longest title 72 bytes). Longer unknown
strings such as PUBTOOLINFO are truncated evidence, not decision metadata.
Check: test (external file format and memory ownership). Test: `test/unit.c` › `test_sfo`.
Source: rewrite memory audit 2026-11-25.

### PKG-3: malformed structure fails closed
Truncated tables, out-of-range offsets, impossible lengths and unreadable entries fail the parse or
entry read. They are never partially trusted as valid metadata.
Check: test. Tests: `test/unit.c` › `test_sfo`, `test_pkg_reader`; fuzz target `test/fuzz/fuzz_pkg.c`.
Source: existing parser design.

### PKG-4: the 4 KiB PKG header is parse input, not retained state
`pkg_open` may read the fixed header while parsing, but the open object keeps only decision fields derived
from it. The full header is not retained for every open pkg. The content-type byte needed for add-on data
classification is distilled explicitly during the parse.
Check: test (memory ownership and external file format). Test: `test/unit.c` › `test_pkg_reader`.
Source: rewrite memory audit 2026-11-25.
