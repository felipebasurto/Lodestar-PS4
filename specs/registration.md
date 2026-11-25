# Registration metadata spec

Governs: registration-time package metadata in `src/main.c`, the distilled metadata
boundary in `src/pkg.c`/`src/pkg.h`, and cached decision data derived from it.
Sources: rewrite memory audit 2026-11-25 and the measured Toshiba library in `specs/pkg.md`.

## Rules

### REG-1: registration keeps distilled decision metadata, not whole SFO tables
The registration representation may keep only the keys it consumes: `TITLE`, `VERSION`,
`APP_VER`, `SYSTEM_VER` and `CATEGORY`. Presence and integer/string type validity are part
of the metadata because missing keys and wrong types have different fallback behavior.
The full bounded `sfo_t` table may be used transiently for validation, but is not embedded
in `title_src_t` or `dlc_src_t`.
Check: test (external file format, memory ownership and behavior parity). Tests:
`test/unit.c` › `test_sfo`, `test_pkg_reader`; compile boundary `test/sdkcheck.sh`.
Source: rewrite memory audit 2026-11-25.

### REG-2: malformed or wrong-type metadata keeps the historical fallback behavior
A missing title falls back to the title id, a missing/invalid `APP_VER` falls back to
`01.00` where the old code did, a missing/invalid `SYSTEM_VER` is zero, and a missing or
non-`ac` category rejects an add-on. Distillation must not turn absence into presence or
wrong types into valid values.
Check: test. Test: `test/unit.c` › `test_pkg_meta`.
Source: existing registration behavior.

## Non-goals

- Changing the external param.sfo parser rules in `specs/pkg.md`.
- Dropping validation of unknown keys; the parser still validates the whole external table.
- Claiming hardware validation without console evidence.
