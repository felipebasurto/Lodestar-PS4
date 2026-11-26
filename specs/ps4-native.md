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

### P4N-17: legacy add-on enablement does not consent to unbounded internal copies
Configuration files before `ver=3` have no add-on mode key. Their `dlc=1` means only that add-ons
are served; it must migrate to `external`, never to `internal`. Internal installation is a choice
only when a `ver=3` configuration or the current panel explicitly names `dlc_mode=internal`.
The same mapping applies when a legacy client sends the boolean `dlc` key without a mode.
Check: read (configuration parsing is system-facing and compiled through `test/sdkcheck.sh`).
Source: firmware 14.00 live run, 2026-11-26: a `ver=2` file migrated to internal mode queued
about 150 GB of Call of Duty add-on copies with under 60 GB free.

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
Drive/links cancellation, worker-quit, per-transfer stop and HTTP cancellation observations must use
atomic loads/stores (or stronger synchronization), never plain shared-word or `volatile` accesses.
Mutexes remain the owner of queues, progress and byte accounting; atomics only publish one-way stop
signals across worker, network and owner threads. Clearing cancellation before a new job and setting
it from a panel/stop action must both be ordered publications.
Check: test through the existing concurrent Drive/links integration harnesses (`test/drive_test.c`,
`test/links_test.c`); read for the ownership split between byte/progress state and stop signals.
Source: transfer-thread cancellation audit.

### P4N-19: legacy payload names are cleanup aliases, not AutoRun launch targets
`lodestar-ps4.bin` and `lodestar-ps4.elf` are the only payload names Lodestar may choose when enabling
GoldHEN AutoRun. Legacy `ps4mount-ps4.bin/.elf` entries are still recognized and removed so an old
installation is replaced rather than duplicated, but a legacy file is never selected as the new AutoRun
target: doing so could silently downgrade the boot payload to PS4Mount. If only a legacy file exists,
the panel reports that no Lodestar payload file is available and asks for the current artifact.
Check: read (the file selection is console-facing and compiled through `test/sdkcheck.sh`; ini replacement
and legacy-entry cleanup are host-tested in `test/unit.c` › `test_autorun`).
Source: Lodestar independence audit, 2026-11-26.

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

### P4N-21: dotfiles and macOS AppleDouble sidecars are not PKG adoption candidates
Directory entries whose name starts with `.` (including macOS AppleDouble sidecars `._*.pkg`, `.pkg`
and hidden dotfiles) are never PKG candidates in `adopt_dir_pass` or `has_pkg`. An AppleDouble
`._*.pkg` sidecar is ignored silently (and counted in scan telemetry) without calling `stat` or
`pkg_open`, avoiding repeated futile USB header reads across both adoption passes. Historical log
lines from older payloads that skipped `._*.pkg` after header read remain informational in
`tools/logcheck.py` (`LOG-5`).
Check: test (real hardware performance regression and external filename input space). Test:
`test/unit.c` › `test_scanfilter`; `adopt_dir_pass` and `has_pkg` integration compiled through
`test/sdkcheck.sh`.
Source: CUSA23827 background-I/O audit, 2026-11-26 (41 `._*.pkg` sidecars opened twice per scan).

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

### P4N-23: offline exFAT allocation mapping is read-only and validates volume integrity
`tools/exfatmap.py` maps physical cluster extents, fragmentation metrics (`extent_count`,
`fragments_per_GiB`, `largest_extent_share`, forward gaps, backward jumps) and pairwise hole-sharing
interleaving for target files directly from a raw exFAT device or image using only the Python standard
library and `os.O_RDONLY`. It validates boot geometry and main/backup boot checksums, active FAT,
allocation bitmap, `0x85`/`0xC0`/`0xC1` directory entry sets and `NoFatChain` vs FAT-chain resolution,
rejecting corrupt geometry, FAT cycles, truncated chains and cross-file cluster overlaps.
Check: test (keep bar 1: external binary filesystem format; keep bar 4: parser input space). Test:
`test/exfatmap_test.py` (synthetic exFAT images).
Source: CUSA23827 physical-allocation measurement design (`a2-report.txt`), 2026-11-26.

## Toshiba reference profile (not a compatibility requirement)

The user's library disk observed on 2026-11-25: external USB 3.0 enclosure, 1.0 TB, exFAT, volume `TOSHIBA`,
about 329.5 GB free. The active library contained over 100 PKG metadata entries when including test residue;
one Black Ops II base pkg was actively downloading and incomplete, so no write or benchmark was performed.
Two tiny add-on pkgs were nonzero files whose headers declared package size zero; one corresponds to the
Uncharted add-on that timed out during the PR #11 hardware run. A `.ps4mount-prueba-fighterz` tree used about
7.4 GB of test residue. These facts motivate validation and cleanup audits, but the implementation must remain
drive-agnostic.

## CUSA23827 external-streaming evidence (2026-11-26)

Hardware-verified facts from firmware 14.00 with the Toshiba USB 3.0 exFAT drive:

- `patch.pkg` is 55,208,181,760 bytes (51.42 GiB) and remains the shared Call of Duty HQ core package.
- The MW3 executable `cod23-cod.elf` mounts exactly `CODMW3CONTENT003/004/005/006`; the initial launcher
  separately mounts seven add-ons, including MW3 campaign and BO6 packages that the MW3 executable does not.
- In a controlled reversible experiment, those four MW3 runtime add-ons were temporarily present as complete
  internal copies while `patch.pkg` remained on USB. The user estimated only a 35-40% improvement: launch,
  match entry, texture streaming and minimap population were still slow.
- The launcher took about 101 seconds after its initial process appeared before `cod23-cod.elf` appeared;
  the later mount of the four MW3 runtime packages took only about 2.6 seconds.
- The temporary internal copies were removed only after the external mount was restored and all twelve
  add-on labels were visible again. Internal free space then measured 159,144,509,440 bytes.
- Package modification times were mostly sequential, not massively overlapping. They are not evidence of
  fragmentation or of contiguous placement.
- An FTP range-read comparison measured about 5.5-5.9 MB/s from both USB and an internal file. This is
  invalid as a disk benchmark because the FTP/network path saturates first; it must not be used as evidence
  that USB and internal storage perform equally.
- Console-side read-only `pread` benchmark (`benchdisk` v1, `bench_ver=1`, `build=0329aaa4fafc+dirty`,
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
  - Concurrent interleaved 1 MiB streams (`usb_conc`, 1/2/4 threads, 3×64 MiB each): `92.590` → `63.644` →
    `46.080 MiB/s` mean (4-stream min `21.505 MiB/s`).
  - Direct USB vs nullfs-mounted patch (`patch_cmp`, 4 paired alternating 32 MiB adjacent windows, same
    `dev=439 ino=4026532727`): `/mnt/usb0/ps4/CUSA23827/patch.pkg` mean `94.074 MiB/s` (sd `7.577`) vs
    `/user/patch/CUSA23827/patch.pkg` mean `94.268 MiB/s` (sd `2.095`).
  - Internal control (`/user/app/CUSA36843/app.pkg`, 9.01 GiB on encrypted UFS `/dev/da0x13.crypt`):
    `79.473 MiB/s` seq, 4 KiB rand mean `16.167 ms` (p50 `12.902 ms`). Not directly comparable to the
    51.42 GiB USB patch because file size, LBA placement, encryption and filesystem differ.
  - `O_DIRECT` probe (`odirect_open=1`, `odirect_read=1`, 4 KiB rand mean `13.198 ms`) is accepted by the
    kernel interface but is not proof of buffer-cache bypass.

Inference, not proof: sequential USB throughput (~92-94 MiB/s) is healthy and nullfs/path indirection shows
no measurable sequential overhead (`94.074` vs `94.268 MiB/s`), so nullfs is not a priority suspect. The
measured ~12 ms 4 KiB seek latency and sharp drop under concurrent streams (`92.6` → `63.6` → `46.1 MiB/s`)
are consistent with mechanical-drive seek contention across the 51.42 GiB core package and large add-ons,
compounded by background scan metadata loops (e.g., 41 macOS `._*.pkg` sidecars and `/api/games`
fingerprint recomputation).

Unknowns that still require console/offline proof:
- Physical exFAT allocation and fragmentation (extent count, backward jumps, gaps and interleaving of
  `patch.pkg` and `CODMW3CONTENT003/004/005/006` on the Toshiba drive).
- Runtime PFS/PFSC/HMAC read amplification (whether PS4 verifies signed 64 KiB outer-PFS HMACs on demand
  and what cache/readahead sizes apply).
- Real runtime I/O access pattern of `cod23-cod.elf` (block sizes, offsets, concurrency across `patch.pkg`
  and the four add-on packages).
- Random 4K/64K nullfs parity, multi-file concurrent throughput (`patch.pkg` + 4 add-ons), cache warm-repeat
  contrast, and scan metadata microbenchmark timings (scheduled for `benchdisk` v2).
- PlayGo locus semantics, the functional effect of the repeated `/mnt/sandbox/download1/CUSA23827`
  `0x809f8500` error, and whether a USB SSD changes cold-boot gameplay materially.

## Open questions requiring console proof

- Whether any safe interface can make BGFT ingest launch tasks without reboot.
- Exact sandbox visibility of host nullfs mounts for emulator/homebrew data on each GoldHEN/firmware pair.
- Whether generated PDB/PBM/PlayGo bytes differ across PS4 firmware families.
- Whether forced unmount of dead USB-backed nullfs layers is safe on every supported firmware.
- Why EA SPORTS FC 27 hit `CE-34224-5` in the PR #11 run after transfer activity; not enough evidence to
  blame the mount design.
