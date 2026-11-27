# PS4-native architecture spec

Governs: the rewrite work on `rewrite/ps4-native`, especially registration, mounting, transfers,
notifications, logging and firmware capability handling.
Sources: PR #11 hardware runs on PS4 firmware 14.00; the Toshiba exFAT library audit of 2026-11-25;
`gasb07/ps4mount` upstream v0.8.0; psdevwiki PKG/Param.sfo/Partitions pages; flatz's BGFT write-ups;
ps4-payload-sdk commit `573b4a0d0d5fa8695cfeee5f5e2f0447c550035d`.

## Purpose

PS4Mount is not a PS5 payload with PS4 filenames. It is a PS4 system-service composition built around
five facts: BGFT launch tasks are ingested at boot, system databases are owned by ShellCore, nullfs mount
layers survive the daemon, USB mount names are not disk identities, and payload capabilities differ by
firmware and loader. Every behavior must be auditable from logs and reproducible host tests.

## Rules

### P4N-1: C is the implementation language
The payload uses C11, the ps4-payload-sdk, direct syscalls and small bounded allocations. Rust or C++ would
add a runtime/toolchain porting problem without improving safety at the kernel, BGFT or nullfs boundary.
Safety comes from module boundaries, strict parsers, host tests, sanitizers and explicit state machines.
Check: read. Source: this rewrite.

### P4N-2: system databases are never written
`app.db`, `addcont.db` and their journals are read-only inputs. Registration and uninstallation go through
system services. A failed or incomplete read is "unknown", never proof of absence.
Check: test (existing policy and database tests). Source: AI-WORKFLOW rule 1.

### P4N-3: user package files are never exposed to a system delete
A delete may remove only generated internal hold files and, when the user deletes the game on the console,
the exact library folder belonging to that game. Decision logic for deletion must be host-testable.
Check: test (keep bar 2: data loss). Source: AI-WORKFLOW rule 2.

### P4N-4: registration, mounting and launchability are separate states
A title can be discovered, validated, prepared, mounted, registered, BGFT-task-written, or ready. Only a
title whose BGFT task survived one console restart is presented as ready. Registration alone is not readiness.
Check: read. Source: PR #11 firmware 14.00 hardware run.

### P4N-5: system-facing paths use canonical PS4 locations
Paths written to BGFT/system metadata use `/user/data`, `/user/app`, `/user/patch` and `/user/addcont` forms.
`/data` may be a payload-local alias but is not blindly treated as interchangeable inside every sandbox.
Emulator/shared-folder mounts are unproven until the target application itself reads them.
Check: read. Source: PR #11 PS4SX2 evidence; psdevwiki mount table.

### P4N-6: firmware support is a capability matrix
SDK offsets, `sceAppInstUtil` exports, registration behavior, loader process context and
GoldHEN integration are independent capabilities. Absence degrades one feature; it does not stop unrelated
features. Unsupported registration fails closed.
Check: read. Source: ps4-payload-sdk; local firmware tests.

### P4N-7: a package with impossible size metadata is not handed to the system installer
A nonzero PKG file whose header declares package size zero is damaged or malformed for install purposes.
It may be shown as a problem, but PS4Mount must not queue it to BGFT. The observed Uncharted timeout
correlates with such a file; causality is not proven.
Check: test (keep bar 5: real hardware regression). Source: Toshiba audit 2026-11-25; PR #11 Uncharted DLC timeout.

### P4N-24: legacy add-on enablement does not consent to unbounded internal copies
Configuration files before `ver=3` have no add-on mode key. Their `dlc=1` means only that add-ons
are served; it must migrate to `external`, never to `internal`. Internal installation is a choice
only when a `ver=3` configuration or the current panel explicitly names `dlc_mode=internal`.
The same mapping applies when a legacy client sends the boolean `dlc` key without a mode.
Check: read (configuration parsing is system-facing and compiled through `test/sdkcheck.sh`).
Source: firmware 14.00 live run, 2026-11-26: a `ver=2` file migrated to internal mode queued
about 150 GB of Call of Duty add-on copies with under 60 GB free.

### P4N-25: add-ons are served from the drive by default
The default add-on mode is `external`: packages are mounted from their drive and nothing of them is
copied to internal storage. Installing them is only what the user chooses.
The user's PS4 ran the add-ons of Modern Warfare 3 and Black Ops 6 this way, with about 70 GB free
internally. Not established: that every game sees a mounted add-on, or that records left by earlier
install attempts played no part (on 2026-10-19 a mounted add-on was seen to be invisible, commit
`ab38c85`).
Check: read. Source: user report, 2026-11-26.

### P4N-18: internal add-on install space is preflighted as a queue, not per file
Before an internal add-on install is submitted, PS4Mount must account for every add-on byte
already submitted in the same payload run plus the current package and the safety margin. If that
aggregate does not fit in `/user`, the package is not submitted. Per-file free-space checks are
insufficient because BGFT copies asynchronously: many individually accepted packages can together
exceed the disk. Bytes conservatively remain reserved for the payload run even when an install
finishes or fails; a later payload load reevaluates the real disk.
Check: read (BGFT completion is system-facing and compiled through `test/sdkcheck.sh`).
Source: firmware 14.00 live run, 2026-11-26.

### P4N-8: kernel-log copying is rate-limited without dropping new information
The first instance of a relevant kernel line is copied. Exact repeats inside a one-minute window are counted
and summarized once; different lines are never suppressed. `klog_all` diagnosis remains available.
Check: test (keep bar 3: time/ordering; real upstream regression). Source: upstream v0.8.0 issue #42.

### P4N-9: notifications are operational, not a progress spam channel
Console popups are reserved for actions required, final outcomes and newly observed failures. Incremental
progress and per-item details of a multi-game batch are written to the panel/log without interrupting gameplay.
A persistent mount failure produces one popup per title per daemon run until that title mounts successfully;
it does not repeat on every scan. Batch completion/restart summaries remain popups.
Check: read. Source: user request 2026-11-25.

### P4N-10: transfers are atomic, resumable and observable
Every pkg transfer writes a temporary file, validates header/category/size as soon as possible, completes
close/write validation, and renames only after success. Partial state is either resumable or collected by a
logged retention rule; active transfers are never cleaned.
Check: test (existing upload/links integration tests). Source: existing transfer design and rewrite.

### P4N-11: library scale is explicit
The payload supports up to 768 titles and 64 scan folders per drive, bounded by fixed memory. When input
exceeds the limit the panel/log says so instead of silently dropping games.
Check: read. Source: upstream v0.7.3.

### P4N-12: audit data must distinguish facts, inference and unknowns
Reports and documentation label hardware-verified facts, host-test evidence, community reverse-engineering,
and unresolved questions. A missing console/drive or failed read is never reported as absence.
Check: read. Source: user requirement for agent-auditable output.

### P4N-13: BGFT task state is parsed once per scan epoch
A scan evaluates titles against one bounded snapshot of BGFT task metadata. The parser reads the canonical
PDB records (`0x7c` title id, exact length-delimited `0x1388` package URL, `0x68` error) with strict length checks; a canonical
field present with the wrong width is malformed, not absent. A malformed or unreadable task is unknown/untrusted,
never proof that registration is healthy. Within the serialized scan/registration epoch, absence and task-id
occupancy may come from that snapshot; positive identity used for mutation is re-read fresh, and uncertain writes
invalidate the epoch as specified in `specs/bgft.md`. New task-id allocation additionally uses
the boot-scoped taken set of `specs/bgft.md`, because system install tasks created inside the
scan epoch may not have directories yet. Fresh fallback reads use exactly the same canonical
health semantics as the snapshot; they differ only in I/O cost.
Check: test (keep bar 1: external file format; keep bar 3: scan ordering). Test: `test/unit.c` ›
`test_bgft_info` and `test_bgft_scan`. Source: performance audit 2026-11-25; flatz BGFT format notes.


### P4N-14: nullfs ownership is explicit and fail-closed
Mount mutation is based on ownership evidence, not on the fact that a target is mounted. Current hold-folder
layers, proven legacy direct-drive layers, foreign layers and unreadable observations are distinct states;
foreign or unknown layers are never force-unmounted. Mount and detach postconditions are defined in
`specs/mount.md` and their decision tables are host-testable.
Check: test (security/data loss). Test: `test/unit.c` › `test_mountplan`. Source: mount ownership rewrite 2026-11-25.

### P4N-15: one scan pass evaluates one package-file fingerprint epoch
Within one `scan_root` pass, a title's package-file fingerprint is computed once and reused
when the same title is kept mounted. The fingerprint includes app/update marker state and
every visible add-on, so recomputing it a few lines later is pure repeated `stat` work,
especially costly on large USB libraries. A file mutation observed after classification belongs
to the next scan epoch; it does not make the current pass half-old and half-new. Persistent
cache lookup semantics and invalidation outside the pass are unchanged.
Check: read (the scan loop is system-facing and compiled through `test/sdkcheck.sh`; the
fingerprint format itself remains covered by `test/titlecache_test.c`).
Source: performance audit of scan_root repeated fingerprinting.


### P4N-16: helper-thread completion is an ordered publication, never a volatile flag
System-service helper threads (InstallAll/TitleDir and UnInstall) publish their result before
clearing their busy state with release ordering; callers read busy/result with acquire ordering.
`volatile` is not synchronization and must not be used as one. A timed-out busy helper still
blocks later helper use until it publishes completion, so its title/input slots are never overwritten.
While busy the result is unknown and logs say pending; zero or an earlier result is never printed
as evidence of the current call.
Check: read (the system-service boundary is compiled through `test/sdkcheck.sh`; host tests cannot
call the PS4 installer). Source: concurrency audit of existing install/uninstall helper threads.

### P4N-17: transfer cancellation and stop signals are ordered atomics
Links cancellation, worker-quit, per-transfer stop and HTTP cancellation observations must use
atomic loads/stores (or stronger synchronization), never plain shared-word or `volatile` accesses.
Mutexes remain the owner of queues, progress and byte accounting; atomics only publish one-way stop
signals across worker, network and owner threads. Clearing cancellation before a new job and setting
it from a panel/stop action must both be ordered publications.
Check: test through the existing concurrent links integration harness (`test/links_test.c`); read
for the ownership split between byte/progress state and stop signals.
Source: transfer-thread cancellation audit.

### P4N-19: PS4Mount payload names are foreign AutoRun entries, not Lodestar cleanup targets
`lodestar-ps4.bin` and `lodestar-ps4.elf` are the only payload names Lodestar may choose or remove when
enabling or disabling GoldHEN AutoRun. `ps4mount-ps4.bin/.elf` entries belong to the independent
PS4Mount-PS4 installation (or are indistinguishable from it), so Lodestar must preserve them verbatim and
must never select one as its AutoRun target. If only a PS4Mount-named file exists, the panel reports that
no Lodestar payload file is available and asks for the current artifact.
Check: read (the file selection is console-facing and compiled through `test/sdkcheck.sh`; ini replacement
and foreign-entry preservation are host-tested in `test/unit.c` › `test_autorun`).
Source: user hardware coexistence requirement, 2026-11-26.

### P4N-20: routine scans yield to gameplay
The routine scan period is 10 seconds while no PS4 title has a sandbox application mount. While any
title is running, the routine period is 120 seconds, because every routine pass revalidates USB package
metadata that competes with game streaming on mechanical drives. This cadence affects only routine
scheduling after a completed scan epoch: manual scans, hard scans and transfer-completion scans still
become due immediately through the scan-request boundary. A scan already in progress is not interrupted,
and the next cadence is decided from the sandbox mount table at the end of that epoch.
Check: test (real hardware performance regression and scan timing/ordering). Test: `test/unit.c` ›
`test_scanreq`; the sandbox-mount integration is compiled through `test/sdkcheck.sh`.
Source: Lodestar background-load audit during CUSA23827 gameplay, 2026-11-26.

### P4N-21: dotfiles and macOS AppleDouble sidecars are ignored, not adopted or deleted
Directory entries whose name starts with `.` (including macOS AppleDouble sidecars `._*.pkg`, `.pkg`
and hidden dotfiles) are never PKG candidates in `adopt_dir_pass` or `has_pkg`. An AppleDouble
`._*.pkg` sidecar is ignored silently (and counted in scan telemetry) without calling `stat` or
`pkg_open`, avoiding repeated futile USB header reads across both adoption passes. Historical log
lines from older payloads that skipped `._*.pkg` after header read remain informational in
`tools/logcheck.py` (`LOG-5`).
AppleDouble sidecars are macOS metadata garbage to Lodestar, but they are still files on the user's
drive. Lodestar does not garbage-collect or delete them: deletion would be a destructive write outside
the only user-file deletion exception allowed by STORE-1, while the measured Toshiba residue is only
82 files / 335,872 bytes and the current filter removes their repeated open/stat cost. Ignoring them
is the permanent behavior, not a temporary conservative default.
Check: test (real hardware performance regression and external filename input space). Test:
`test/unit.c` › `test_scanfilter`; `adopt_dir_pass` and `has_pkg` integration compiled through
`test/sdkcheck.sh`.
Source: CUSA23827 background-I/O audit, 2026-11-26 (41 `._*.pkg` sidecars opened twice per scan);
`evidence/cusa23827/layoutscan.json` archived in commit
`ef98120b6524ea959fcd2a0e5e234333ba7ae0ac` (82 AppleDouble files, 335,872 bytes).

### P4N-22: scan epochs and `/api/games` emit compact key=value telemetry without changing behavior
Every executed scan epoch emits one `[scan] epoch ...` key=value log line reporting `seq`, `trigger`,
`gameplay`, `wall_ms`, `usb_stat`, `usb_open`, `usb_read`, `usb_bytes`, `adopt_cand`,
`adopt_appledouble`, `fp_app`, `fp_patch`, `fp_badpatch`, `fp_ac`, `hold_ready`, `bgft_dirs`,
`bgft_pdb_reads`, `bgft_pdb_bytes` and `mount_reads`. Every `GET /api/games` request emits one
`[api] games ...` key=value log line reporting `wall_ms`, `titles`, `usb_stats`, `fp_app`, `fp_patch`,
`fp_ac` and `cache_miss`. Telemetry is strictly observational: it must not alter scan semantics,
cadence, registration or `/api/games` freshness (no snapshot serving), and emits nothing while idle
between scans or without API requests.
Check: test (public log contract, trigger classification and BGFT read accounting). Test:
`test/unit.c` › `test_telemetry`, `test_scanreq`, `test_bgft_scan`; `main.c` hooks compiled through
`test/sdkcheck.sh`.
Source: CUSA23827 background-I/O instrumentation plan, 2026-11-26.

### P4N-23: offline exFAT allocation mapping is read-only, alignment-safe and validates volume integrity
`tools/exfatmap.py` maps physical cluster extents, fragmentation metrics (`extent_count`,
`fragments_per_GiB`, `largest_extent_share`, forward gaps, backward jumps) and pairwise hole-sharing
interleaving for target files directly from a raw exFAT device or image using only the Python standard
library and `os.O_RDONLY`. It validates boot geometry and main/backup boot checksums, active FAT,
allocation bitmap, `0x85`/`0xC0`/`0xC1` directory entry sets and `NoFatChain` vs FAT-chain resolution,
rejecting corrupt geometry, FAT cycles, truncated chains and cross-file cluster overlaps.
The parser's public byte offsets and lengths remain exact, but every physical read goes through a
512-byte-aligned window: the start is padded down, the end is padded up, and the requested bytes are
cropped out after reading. This is required for macOS raw `/dev/rdisk*` devices, which reject
unaligned offset/length pairs with `EINVAL`; it must not change results on regular image files. A
short read is fatal only when the requested bytes themselves are missing, never merely because the
aligned padding crosses end-of-file.
Check: test (keep bar 1: external binary filesystem format; keep bar 4: parser input space). Test:
`test/exfatmap_test.py` (synthetic exFAT images, including a strict raw-device-like `pread` that
rejects unaligned offset/length pairs).
Source: CUSA23827 physical-allocation measurement design (`a2-report.txt`), 2026-11-26; macOS raw
device `EINVAL` failure observed while reading the Toshiba exFAT partition.

### P4N-26: physical placement is operator guidance, not a runtime control plane
The official recommendation for a large USB library whose games stream a patch and several add-ons
concurrently is a USB SSD: solid-state storage has no moving head and therefore removes the mechanical
seek component of this access pattern. Mechanical USB HDDs remain supported, but a large library with
patch+add-on concurrency must be expected to degrade streaming when those files are widely separated;
no payload ordering can move the user's data on the platter. This recommendation is operational
guidance, not a measured claim that a specific SSD/console/game triple has already been benchmarked.
Lodestar must not invent a mount-time fragmentation or physical-separation report. The file APIs
available to the payload expose logical files, not exFAT cluster allocation. No hardware evidence
establishes a supported payload interface for reading the live FAT of a mounted exFAT volume, and a
raw-device shortcut would be unvalidated against the mounted filesystem's own metadata cache.
Physical allocation is therefore unknown at runtime unless future console evidence proves a safe
interface; `tools/exfatmap.py` remains an offline host diagnostic for an unmounted disk or image.
Lodestar also must not hide, limit or reorder user-visible add-ons because of inferred disk layout.
Reordering mount operations does not change physical placement, and suppressing concurrent add-ons
can break the game. The runtime mitigations are limited to work Lodestar owns: gameplay scan cadence
(P4N-20), sidecar filtering (P4N-21), one-fingerprint scan epochs (P4N-15) and external add-ons by
default (P4N-25).
Check: read. Source: CUSA23827 evidence bundle archived in commit
`ef98120b6524ea959fcd2a0e5e234333ba7ae0ac` and the design decision of 2026-11-26.

### P4N-27: Lodestar is side-by-side with PS4Mount, but only one mount daemon runs
Lodestar's operational identity is distinct from PS4Mount-PS4: state in `/data/lodestar`, hold files in
`/system_data/lodestar`, the default USB scan folder `Lodestar`, internal packages in `/data/Lodestar`,
web panel port 8780, helper relay base port 8781, emulator source folder `Lodestar/emulators`, and native
panel title id `LODS00001`. Lodestar does not read, migrate, adopt or delete PS4Mount's state in
`/data/ps4mount`, `/system_data/ps4mount`, the `PS4Mount` scan folder or panel title `PSMT00001`; those
files and mount layers are foreign and fail closed.

The loopback instance lock on port 9032 remains deliberately shared with PS4Mount-PS4. It is a
cross-product safety interlock, not shared product identity: both daemons mutate the same global mount
table, BGFT tasks, system installer state and kernel-log consumer, so they must alternate rather than run
at once. Stopping one payload and loading the other must preserve each product's own library and state.

The organized package bytes in `<drive>/ps4/<TITLE_ID>/` are user/library data, not product-private state.
Lodestar may scan and serve them, but without Lodestar ownership evidence it must treat their mount layers and
management records conservatively. A user-initiated console delete follows the normal delete contract; daemon
cleanup must never use PS4Mount's `.src`, `.removed` or hold files as Lodestar evidence. When PS4Mount is
installed or enabled in GoldHEN AutoRun, P4N-32 holds automatic registration and library migration so Lodestar
does not re-register or move PS4Mount's shared library.
Check: test for port/manifest and ownership decision tables; read for the console-wide service interlock.
Source: user hardware coexistence requirement, 2026-11-26.

### P4N-28: panel auto-launch requires a valid foreground user; never launch as user 0
`sceUserServiceInitialize` is called with a valid priority parameter (`0x2BC`, normal priority) and its return
code is logged (`0` ok, `0x80960003` already initialized in the borrowed host process, any other code is a
failure). Before auto-opening the native panel app (`LODS00001`) on a first start, the foreground user ID
variable is initialized to `-1` (never `0`) and queried with `sceUserServiceGetForegroundUser`. `sceLncUtilLaunchApp`
must never be called if the query returns a non-zero status or an invalid user ID (`<= 0xFF`, which rejects
`0`, `-1`, all negative values, `0xFE` `USER_ID_EVERYONE`, and `0xFF` `USER_ID_SYSTEM`), or while any game
or the panel app is already open (or open-app inspection fails), or unless `app.db` freshly confirms the
panel app is registered. The exact return code and user value are logged. When no valid foreground user is
available, Lodestar skips auto-opening and leaves clear instructions in the log, console notification and
panel notices telling the user to open Lodestar manually or use the web panel.
The user predicate is REG-6. Check: test (system boundary, database-corruption regression on firmware 14.00).
Test: `test/fw_sim.c` (`boot_launch_user`, `boot_panel_app_plan`, built with `-DSTRICT_FIRST_RUN_SAFETY`);
compile boundary `test/sdkcheck.sh`.
Source: firmware 14.00 rc.3 hardware incident (2026-11-27): auto-opening with unchecked `user = 0` was
followed 200 ms later by `DbCorruptionOccured ... app.db` / `no such table: tbl_appbrowse_0000000000`.

### P4N-29: first-run library adoption requires explicit user consent, never a UI timeout
On a first start (no `config.txt` and no prior Lodestar `.src` or `catalog-*` state), Lodestar waits
indefinitely for the user to explicitly save settings (`POST /api/config`); the scan loop stays closed
(`boot_scan_gate`) and the main loop has no timeout of its own.
No elapsed timeout, panel absence, panel close, or native-app crash may auto-save defaults or trigger adoption
or registration of preexisting games (whether the library has 1 game or 20+ games). Opening or polling the
panel is strictly observational and never completes first-run consent. Once the user explicitly saves settings
(or on a subsequent run where `config.txt` or prior Lodestar state already exists), scans adopt and register
packages according to the saved settings, through the pilot breaker of REG-8.
Check: test (unwanted mass registration / BGFT write prevention, state/time ordering, real firmware 14.00
regression). Test: `test/fw_sim.c` (the rc.3 timeline, built with `-DSTRICT_FIRST_RUN_SAFETY`); panel copy
in `test/web/panel.test.js`.
Source: firmware 14.00 rc.3 hardware incident (2026-11-27): after the panel app crashed on first start,
the 120-second timeout auto-saved defaults and registered 20 preexisting games in 4 groups.

### P4N-30: system database corruption markers are detected read-only without touching databases
`app.db` remains strictly read-only (`mode=ro&nolock=1`); Lodestar never writes, creates, repairs or deletes
any file under `/system_data/priv/mms/`. `mode=ro&nolock=1` acquires no locks that could fail ShellCore's
writes and cannot modify `app.db`; its only trade-off is possible transient inconsistent reads on the reader
side, which `db.c` already handles fail-closed (`db_confirms_registered` and `db_confirms_unregistered`).
The system's `*.db-corrupt` markers are only looked at with read-only `stat()` by the REG-7 latch
(`src/dbguard.c`), at startup and while running, never modified or deleted.
Check: test (system boundary, read-only database contract). Test: `test/db_test.c`, `test/dbguard_test.c`,
`test/policy.sh`; compile boundary `test/sdkcheck.sh`.
Source: firmware 14.00 rc.3 hardware incident (2026-11-27): `app.db` passed `pragma quick_check=ok` while
`/system_data/priv/mms/app.db-corrupt` existed.

### P4N-31: busy instance lock reports real lock state without blaming PS4Mount
The shared loopback lock on port 9032 (P4N-27) is preserved unchanged. When acquiring it fails, Lodestar
logs the port, `errno` and `pid`, probes loopback panel port 8780 read-only (`GET /api/agent`) with a short
timeout to check whether another Lodestar instance is already answering, and reports clearly whether Lodestar
itself is already running, another active instance holds lock port 9032, or lock creation failed for another
reason. It must not specifically claim PS4Mount/USB Mount is running when the holder may be Lodestar itself.
Check: test (conflict classification in `test/unit.c` › `test_lock_conflict`); read for loopback probe and
socket error handling (`test/sdkcheck.sh`).
Source: firmware 14.00 rc.3 hardware incident (2026-11-27): a duplicate Lodestar load reported that PS4Mount
was running because both share port 9032.

### P4N-32: PS4Mount coexistence guard blocks automatic library registration
Lodestar detects read-only whether PS4Mount (`Vld439/PS4Mount-PS4`) is installed or enabled in GoldHEN
AutoRun: payload files `ps4mount-ps4.bin` / `ps4mount-ps4.elf` under `/data/GoldHEN/payloads` or
`/data/payloads` (real files >= 4 KiB with ELF or payload magic), installed panel/shortcut directories
`PSMT00001` or `PMNT00001` under `/user/app`, or an active non-comment `ps4mount-ps4.bin` / `ps4mount-ps4.elf`
line in `/data/GoldHEN/payloads.ini`. Leftover `/data/ps4mount` or `/system_data/ps4mount` state alone is
recorded as `ps4mount_state` for visibility without tripping the block, because Lodestar never deletes
PS4Mount files (P4N-27).

While PS4Mount is installed or active in AutoRun, Lodestar holds automatic registration, version-mismatch
re-registration, and `migrate_organized` library moves so it does not re-register or move `<drive>/ps4/<TID>`
titles already managed by PS4Mount. An explicit single-title Add (`POST /api/mount?title_id=<TID>`) from
the panel remains allowed for that requested title alone. Lodestar emits a Spanish/English notice and
exposes the read-only `coexist` object in `GET /api/status` and `GET /api/games`.
Check: test (keep bar 2: data/system safety; keep bar 5: cross-product conflict). Test: `test/env_test.c`,
`test/web/panel.test.js`; compile boundary `test/sdkcheck.sh`.
Source: environment coexistence hazard review (`hazards/environment`), 2026-11-27.

## Toshiba reference profile (not a compatibility requirement)

The user's library disk observed on 2026-11-25: external USB 3.0 enclosure, 1.0 TB, exFAT, volume `TOSHIBA`,
about 329.5 GB free. The active library contained over 100 PKG metadata entries when including test residue;
one Black Ops II base pkg was actively downloading and incomplete, so no write or benchmark was performed.
Two tiny add-on pkgs were nonzero files whose headers declared package size zero; one corresponds to the
Uncharted add-on that timed out during the PR #11 hardware run. A `.ps4mount-prueba-fighterz` tree used about
7.4 GB of test residue. These facts motivate validation and cleanup audits, but the implementation must remain
drive-agnostic.

## CUSA23827 external-streaming evidence (2026-11-26)

Source key for the v2 console run and offline disk-layout bundle. Every file below is archived in
commit `ef98120b6524ea959fcd2a0e5e234333ba7ae0ac`:

- `[v2-bench]`: `evidence/cusa23827/benchdisk-v2-results.txt` (`bench_ver=2`, firmware 14.00,
  run `1795722237-45`, `status=ok`, `errors=0`, `short_reads=0`, normal cached I/O without a cache
  purge; `O_DIRECT` probe not accepted as proof of cache bypass).
- `[exfat-map]`: `evidence/cusa23827/cusa23827-exfat-map.json` and
  `evidence/cusa23827/exfatmap-stdout.txt` (offline read-only allocation map).
- `[layout-scan]`: `evidence/cusa23827/layoutscan.json` (free-space runs and AppleDouble inventory).
- `[mac-layout-bench]`: `evidence/cusa23827/layoutbench.txt` (same Toshiba disk attached to the Mac).
- `[fsck]`: `evidence/cusa23827/fsck-exfat.txt` (`fsck_exfat -n`, exit 0).

### FACT: hardware-verified or directly measured observations

- `patch.pkg` is 55,208,181,760 bytes (51.42 GiB) and remains the shared Call of Duty HQ core package.
- The MW3 executable `cod23-cod.elf` mounts exactly `CODMW3CONTENT003/004/005/006`; the initial launcher
  separately mounts seven add-ons, including MW3 campaign and BO6 packages that the MW3 executable does not.
- In a controlled reversible experiment, those four MW3 runtime add-ons were temporarily present as complete
  internal copies while `patch.pkg` remained on USB. The user estimated only a 35-40% improvement: launch,
  match entry, texture streaming and minimap population were still slow. The 35-40% figure is a user
  estimate, not an instrumented gameplay measurement.
- The launcher took about 101 seconds after its initial process appeared before `cod23-cod.elf` appeared;
  the later mount of the four MW3 runtime packages took only about 2.6 seconds.
- The temporary internal copies were removed only after the external mount was restored and all twelve
  add-on labels were visible again. Internal free space then measured 159,144,509,440 bytes.
- Package modification times were mostly sequential, not massively overlapping. They are not evidence of
  fragmentation or of contiguous placement.
- An FTP range-read comparison measured about 5.5-5.9 MB/s from both USB and an internal file. This is
  invalid as a disk benchmark because the FTP/network path saturates first; it must not be used as evidence
  that USB and internal storage perform equally.
- Console-side read-only `pread` benchmark v1 (`bench_ver=1`, `build=0329aaa4fafc+dirty`,
  `sdk=573b4a0d`, `seed=0xc0de000000023827`, measured on `2026-11-26T15:31:13Z`, run `1795707073-45`,
  `status=ok`, `errors=0`, `short_reads=0`, `total_bytes=1698037760`, normal cached I/O over unique 16 KiB
  granules without cache purge; artifacts SHA-256 `build/benchdisk.elf` =
  `e8da823260c3b6b967f0af9fd6f7d1ad2db2d35317b8efaa3093d472d8409d13`, `build/benchdisk.bin` =
  `3774c495a84de12b1c16a8e1b4a0aafe3f6eac2811748e148fd1c0d0af49e226`) on
  `/mnt/usb0/ps4/CUSA23827/patch.pkg` (`exfatfs`, `/dev/da1s1`):
  - Sequential 1 MiB (`usb_seq`, 4×32 MiB/region): `lo` mean `60.499 MiB/s` (rep 0 `8.021`, reps 1-3
    `74.7-79.6`), `mid` mean `91.843 MiB/s`, `hi` mean `92.788 MiB/s`.
  - Random reads (`usb_rand`): 4 KiB mean `12.062 ms` (p50 `11.990 ms`, p95 `17.969 ms`, `0.324 MiB/s`);
    64 KiB mean `14.213 ms` (`4.401 MiB/s`); 1 MiB mean `37.316 ms` (`29.873 MiB/s`); 8 MiB mean
    `99.120 ms` (`80.841 MiB/s`).
  - Concurrent interleaved 1 MiB streams inside the same patch file (`usb_conc`, 1/2/4 threads,
    3×64 MiB each): `92.590` → `63.644` → `46.080 MiB/s` mean (4-stream min `21.505 MiB/s`).
  - Direct USB vs nullfs-mounted patch (`patch_cmp`, 4 paired alternating 32 MiB adjacent windows, same
    `dev=439 ino=4026532727`): `/mnt/usb0/ps4/CUSA23827/patch.pkg` mean `94.074 MiB/s` (sd `7.577`) vs
    `/user/patch/CUSA23827/patch.pkg` mean `94.268 MiB/s` (sd `2.095`).
  - Internal control (`/user/app/CUSA36843/app.pkg`, 9.01 GiB on encrypted UFS `/dev/da0x13.crypt`):
    `79.473 MiB/s` seq, 4 KiB rand mean `16.167 ms` (p50 `12.902 ms`). Not directly comparable to the
    51.42 GiB USB patch because file size, LBA placement, encryption and filesystem differ.
- Console-side `benchdisk` v2 `[v2-bench]`:
  - Single-stream control (`usb_conc streams=1`): mean `93.120 MiB/s` (3 samples, min `91.592`, max
    `95.203`). Same-file concurrency lowered this to `61.980 MiB/s` at 2 streams and `56.576 MiB/s`
    at 4 streams.
  - Real multi-file streaming (`multifile_seq1m`): `patch.pkg` plus all four MW3 add-ons open at once,
    alternating 1 MiB reads, aggregate mean `22.469 MiB/s` (sd `0.353`, min `22.034`, max `22.899`).
  - Real multi-file random 64 KiB (`multifile_rand64k`): mean latency `93.783 ms`, p50 `88.859 ms`,
    p95 `176.821 ms`, max `241.290 ms`, aggregate mean `3.187 MiB/s`.
  - Direct-vs-nullfs random parity (`patch_rand_cmp`): at 4 KiB, USB mean `13.181 ms` vs nullfs mean
    `12.362 ms`; at 64 KiB, USB mean `13.238 ms` vs nullfs mean `13.584 ms`. No meaningful nullfs
    random-read penalty was measured.
  - Warm repeats: sequential 1 MiB repeat speedup `×2.660` (`85.667` → `227.873 MiB/s`); random 4 KiB
    p50 speedup only `×1.091` and random 64 KiB p50 speedup `×0.994` (that is, no useful small-random
    repeat speedup).
  - Scan metadata microbenchmark (one `stat` + `open` + 4 KiB `pread` per operation): 41 sidecar
    files read in two passes cost 82 operations / 335,872 bytes / `6.214 ms` total; one pass over
    102 library PKG headers cost `1.449 s` (`14.201 ms` mean per operation, p95 `25.908 ms`).
- Offline exFAT allocation map `[exfat-map]`:
  - `patch.pkg`: `extent_count=65`, `contiguous=False`, `fragments_per_GiB=1.245`,
    `largest_extent_share=0.0376`, `forward_gaps=59` totaling 124,725,755,904 bytes and
    `backward_jumps=5` totaling 80,233,234,432 bytes. Its first extent starts at volume byte
    389,394,464,768.
  - `CODMW3CONTENT003/004/005/006`: extent counts `14`, `20`, `10` and `8`; no backward jumps; forward
    gap totals only 3,670,016 / 5,242,880 / 1,835,008 / 1,703,936 bytes. They are therefore much closer
    to contiguous than the patch, though not strictly one-extent files.
  - There is no pairwise extent overlap and no hole-sharing interleaving between `patch.pkg` and any of
    the four add-ons. The closest positive gaps from the patch to `003/004/005/006` are
    425,644,261,376 / 402,086,821,888 / 441,899,548,672 / 452,538,925,056 bytes respectively: even
    the nearest patch/add-on extents are separated by roughly 402-453 GB on this volume.
  - `patch.pkg` extents span volume bytes 389,394,464,768 through 489,095,168,000; the four add-ons
    occupy the roughly 891-953.5 GB region.
- Filesystem/volume state `[fsck]`, `[layout-scan]`:
  - `fsck_exfat -n` reports the volume `TOSHIBA` OK, exit 0.
  - Free space is 86.73 GiB on the 1 TB-class volume (about 91% full), pulverized into 1,208 free runs;
    the largest free run is 4.024 GiB.
  - The library contains 82 macOS AppleDouble `._*` files totaling 335,872 bytes.
- Host independence check `[mac-layout-bench]`: the same Toshiba disk on the Mac read the patch alone at
  `99.1`/`98.8 MiB/s` in two sequential runs, versus `60.7`/`60.8 MiB/s` when five files were interleaved
  in 8 MiB visits.

### INFERENCE: interpretations supported by those facts, not direct proof

- Mechanical seek contention across physically separated files is now the primary explanation for the
  CUSA23827 streaming degradation. The evidence chain is strong but still indirect: multi-file throughput
  collapses to `22.469 MiB/s` while single-stream throughput is about `93 MiB/s`; even the nearest patch
  and add-on extents are separated by 402-453 GB; the same host-independent penalty appears on macOS;
  and direct/nullfs random parity removes nullfs as a priority suspect.
- The 65 extents of `patch.pkg` are large enough (mean roughly 850 MB decimal) that intra-patch sequential
  streaming can remain healthy. The measured catastrophic case is not ordinary fragmentation inside one
  file but the patch↔add-on head movement caused by concurrent multi-file streaming.
- Background scans are a secondary, controllable cost rather than the root cause: 102 real PKG
  `stat`+`open`+4 KiB-read operations cost about 1.45 seconds per pass on this drive, while AppleDouble
  residue is tiny. P4N-20/P4N-21 reduce that cost; they cannot repair the physical layout.
- A USB SSD should remove the mechanical head-seek component because it has no moving head, and is the
  correct operational recommendation for this access pattern. The size of the gameplay improvement on this
  console/game pair is not measured here.
- Copying add-ons internally is not a general solution: the reversible experiment improved the user's
  perceived experience only partially while `patch.pkg` remained on USB, and large internal copies create
  their own storage risk.

### UNKNOWN: questions still open

- The real runtime I/O access pattern of `cod23-cod.elf` (block sizes, offsets, threading and exact
  concurrency across `patch.pkg` and the four add-on packages).
- Runtime PFS/PFSC/HMAC read amplification (whether PS4 verifies signed 64 KiB outer-PFS HMACs on demand
  and what cache/readahead sizes apply).
- The gameplay A/B result for Legacy vs HEAD (phase C has not been run; no conclusion may be claimed yet).

## Open questions requiring console proof

- Whether any safe interface can make BGFT ingest launch tasks without reboot.
- Exact sandbox visibility of host nullfs mounts for emulator/homebrew data on each GoldHEN/firmware pair.
- Whether generated PDB/PBM/PlayGo bytes differ across PS4 firmware families.
- Whether forced unmount of dead USB-backed nullfs layers is safe on every supported firmware.
- Why EA SPORTS FC 27 hit `CE-34224-5` in the PR #11 run after transfer activity; not enough evidence to
  blame the mount design.
