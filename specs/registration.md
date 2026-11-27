# Registration metadata spec

Governs: registration-time package metadata in `src/main.c`, the distilled metadata
boundary in `src/pkg.c`/`src/pkg.h`, and cached decision data derived from it.
Sources: rewrite memory audit 2026-11-25 and the measured Toshiba library in `specs/pkg.md`.

## Rules

### REG-1: registration keeps distilled decision metadata, not whole SFO tables
The registration representation may keep only the keys it consumes: `TITLE`, `VERSION`,
`APP_VER`, `SYSTEM_VER` and `CATEGORY`. Presence and integer/string type validity are part
of the metadata because missing keys and wrong types have different fallback behavior.
`TITLE` keeps the display width of the parser. Version fields keep the eight-byte width
already used by registration/panel consumers; category keeps five bytes, enough to represent
every valid category exactly while a longer malformed value can never truncate into equality
with one. Type validity remains a property of the original field, not of the truncated copy.
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

### REG-4: a known registered-version mismatch is not proof that the update is bad
When PS4Mount has already queued or started the remove/re-add repair because the console's
registered `APP_VER` differs from the update version, a launch-version disagreement for that same
title is circular evidence and must not write a `.badpatch` mark. The repair is allowed to try the
update. Only a fresh disagreement outside that repair, or registration repeatedly failing with the
update, may classify the update as refused.
Check: read (kernel-log/registration ordering is system-facing and compiled through `test/sdkcheck.sh`).
Source: firmware 14.00 Uncharted `CUSA02320` run, 2026-11-24: a valid `gp` 01.02 update was marked
bad while the registered 01.00/expected 01.02 repair was already pending.

### REG-5: a hard scan invalidates exactly the refused-update marks
A hard scan must remove every state-file name ending exactly in `.badpatch`, no more and no less.
The suffix check must include the dot; a shorter comparison that never matches, or a looser one
that can remove unrelated user state, violates the hard-scan contract.
Check: test (persistent state boundary and deletion correctness). Test: `test/badpatch_test.c`.
Source: firmware 14.00 hard rescan 2026-11-26: `CUSA02320.badpatch` survived the intended
invalidation because the implementation compared only eight trailing bytes.

### REG-3: registration-time add-on arrays grow by demand within the fixed cap
The transient `dlc_src_t` array of a title is owned by that `title_src_t` and released by
`src_close`. It must not reserve `MAX_DLC` descriptors when the first add-on is seen: the
first allocation is bounded to one mapped page for the current descriptor stride, growth is
doubling and capped at `MAX_DLC`, and each candidate slot is explicitly zeroed before use.
Allocation failure is logged and leaves the remaining add-ons unprocessed; it is never
interpreted as proof that they are absent or deleted. The ownership/copying rule must remain
safe because open `pkg_t` values are plain movable descriptors, not self-referential objects.
Check: test (memory ownership and boundary arithmetic). Test: `test/unit.c` › `test_dlcvec`;
compile boundary `test/sdkcheck.sh`.
Source: rewrite memory audit 2026-11-25.

### REG-6: registration staging, open-app detection and version-mismatch re-add fail closed
- `open_apps` / `open_games` must return a negative error when `sysctl` or buffer allocation fails rather
  than returning `0` ("no apps open"), so staging masks, panel auto-launch, restore and migration fail closed.
- `install_all` must return immediately after unmasking if `install_all_start` fails instead of waiting 30 s
  and logging stale installer status.
- `mask_user_app` must abort if `unmask_leftover()` could not unmask `/user/app`; `unmask_user_app` must
  keep `g_masked` aligned with `user_app_masked()` and must not clear `STAGE_DIR` while the mask remains active.
- `enforce_patch_sfo` must ignore empty `sfo_len` and compare `param.sfo` files larger than 8 KiB via mapped
  allocation instead of rewriting them on every scan.
- `scan_root` must require `db_confirms_registered(tid)` before queuing a version-mismatch remove/re-add so
  a stale `app.db` cache cannot trigger destructive re-registration.
Check: read (system-facing kernel/DB boundary compiled through `test/sdkcheck.sh`).
Source: upstream vs Lodestar focused registration audit, 2026-11-27.

## Non-goals

- Changing the external param.sfo parser rules in `specs/pkg.md`.
- Dropping validation of unknown keys; the parser still validates the whole external table.
- Claiming hardware validation without console evidence.
