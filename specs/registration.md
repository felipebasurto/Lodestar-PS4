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

### REG-6: user-dependent system calls require a verified non-zero foreground user
Before launching an application (`sceLncUtilLaunchApp`) or queuing a user-owned installer
task (`install_pkg` / BGFT), Lodestar must initialize the output user ID to an invalid
sentinel (`-1`), check the return code of `sceUserServiceGetForegroundUser`, and verify
that the returned user ID is strictly valid (`rc == 0`, `user_id > 0`, and neither
`SCE_USER_SERVICE_USER_ID_EVERYONE` `0xFE` nor `SCE_USER_SERVICE_USER_ID_SYSTEM` `0xFF`).
Launching or installing with user `0` or an unchecked/failed user query is forbidden:
on PS4, launching with `user_id = 0` causes ShellCore to look up `tbl_appbrowse_0000000000`
in `app.db`, fail with `no such table`, emit `DbCorruptionOccured` and write
`/system_data/priv/mms/app.db-corrupt`.
The one function that answers it is `foreground_user_id` in `src/main.c` (rule in
`boot_fg_user_valid`, `src/bootplan.h`); the panel-app launch (P4N-28), the DLC install and a received
install all go through it, and `test/policy.sh` rejects any other call of the query.
Check: test (keep bar 2: data loss; keep bar 5: real hardware regression). Test:
`test/fw_sim.c` (built with `-DSTRICT_FIRST_RUN_SAFETY`); compile boundary `test/sdkcheck.sh`.
Source: firmware 14.00 Lodestar 3.1.0-rc.3 incident (`tbl_appbrowse_0000000000`).

### REG-7: system database corruption markers and klog events latch an immediate halt
If `/system_data/priv/mms/app.db-corrupt` (or `appinfo.db-corrupt` / `addcont.db-corrupt`)
exists on disk, or the kernel log emits `DbCorruptionOccured` (or an equivalent LSDB/DbRecovery
corruption event), Lodestar immediately latches a persistent-for-this-run corruption halt.
While halted, Lodestar must not start or continue any game registration, re-registration,
uninstall, BGFT install, or panel-app auto-launch. Lodestar must never attempt to repair,
delete, rename, or modify any file under `/system_data/priv/mms/`; all markers and databases
are preserved untouched for inspection. Lodestar emits an explicit warning in the log,
console notifications, `/api/status`, `/api/games`, and both web panels telling the user
that the system database is flagged as damaged and to stop registering and restart/recover
before continuing. The scan loop obeys it through `boot_scan_gate` (`src/bootplan.h`), which
puts a flagged database before the first-start wait.
Check: test (keep bar 2: data loss; keep bar 5: real hardware regression). Test:
`test/dbguard_test.c`, `test/fw_sim.c`, `test/web/panel.test.js`, `test/logcheck_test.sh`; compile
boundary `test/sdkcheck.sh`.
Source: firmware 14.00 Lodestar 3.1.0-rc.3 incident (`app.db-corrupt` and repeated
`DbCorruptionOccured` during subsequent `AppInstallAll` batches).

### REG-8: first-start and post-migration registration circuit breaker
A first start never saves default settings or adopts a library by itself (P4N-29).
Registration begins only after the user explicitly saves settings (or explicitly requests a
title). On a first start after settings are saved, or after a configuration/storage migration, a registration circuit breaker caps
registration to at most one pilot title and validates post-registration state (no corruption
latch, fresh `db_confirms_registered` for the pilot title, and `/user/app` proven unmasked).
If validation fails, the breaker trips and halts further registration. If validation succeeds
and more titles remain pending, Lodestar holds the remaining titles until explicit user
continuation (such as a manual scan or explicit add) rather than bulk-registering unattended.
Check: test (keep bar 2: data loss; keep bar 3: ordering; keep bar 5: real hardware
regression). Test: `test/dbguard_test.c`; compile boundary `test/sdkcheck.sh`.
Source: firmware 14.00 Lodestar 3.1.0-rc.3 incident (120 s timeout auto-saved defaults and
registered ~20 titles in 4 batches while the panel app was crashing and `app.db-corrupt` was set).

### REG-9: staging errors and DB errors prioritize restoring `/user/app` and abort the batch
Staging must not mask `/user/app` while any application (including the panel app) is open.
If any database corruption signal or database-layer installer error (`0x80F5xxxx` / `0x809C11xx`)
appears during staging or `AppInstallAll`, Lodestar immediately aborts the wait, prioritizes
unmounting staged child mounts and `/user/app` (gentle retries followed by `MNT_FORCE`), and
aborts the batch without running `register_finish` or subsequent groups. If `/user/app`
cannot be proven unmasked, Lodestar must keep its masked flag set, leave `STAGE_DIR` intact
rather than clearing it while mounted, report the fault, and abort the batch.
Check: test (keep bar 2: data loss; keep bar 3: ordering). Test: `test/dbguard_test.c`;
compile boundary `test/sdkcheck.sh`.
Source: forensic staging/unmount race audit on firmware 14.00 incident.

### REG-10: operational inventory is persisted under `/data/lodestar` before registration
Before registration operations, Lodestar atomically writes its own operational inventory to
`/data/lodestar/inventory.tsv` recording title IDs, source paths, mount mode, base and
effective versions, patch/add-on metadata, and operational state. Entries for titles on
disconnected drives are preserved across scans. This file lives strictly under `/data/lodestar`
and never copies, reads for writeback, or writes any system database under `/system_data/priv/mms/`.
Check: test (keep bar 1: external file format; keep bar 2: recovery/data safety). Test:
`test/dbguard_test.c`; compile boundary `test/sdkcheck.sh`.
Source: post-rebuild recovery requirement after firmware 14.00 database rebuild.

### REG-11: open-app detection, param.sfo checks and version-mismatch re-add fail closed
- `open_apps` / `open_games` return a negative value when `sysctl` or the buffer allocation fails instead of `0`
  ("no apps open"), so staging masks, the panel auto-launch, restore and migration fail closed.
- `enforce_patch_sfo` ignores an empty `sfo_len` and compares a `param.sfo` larger than 8 KiB through mapped
  allocation instead of rewriting it on every scan.
- `scan_root` requires `db_confirms_registered(tid)` before queuing a version-mismatch remove and re-add, so a stale
  `app.db` cache cannot trigger a destructive re-registration.
Check: read (system-facing kernel/DB boundary compiled through `test/sdkcheck.sh`).
Source: upstream vs Lodestar focused registration audit, 2026-11-27.


## Non-goals

- Changing the external param.sfo parser rules in `specs/pkg.md`.
- Dropping validation of unknown keys; the parser still validates the whole external table.
- Claiming hardware validation without console evidence.
