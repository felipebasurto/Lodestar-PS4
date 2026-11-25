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
SDK offsets, `sceAppInstUtil` exports, registration behavior, fusefs/NAS support, loader process context and
GoldHEN integration are independent capabilities. Absence degrades one feature; it does not stop unrelated
features. Unsupported registration fails closed.
Check: read. Source: ps4-payload-sdk; local firmware tests.

### P4N-7: a package with impossible size metadata is not handed to the system installer
A nonzero PKG file whose header declares package size zero is damaged or malformed for install purposes.
It may be shown as a problem, but PS4Mount must not queue it to BGFT. The observed Uncharted timeout
correlates with such a file; causality is not proven.
Check: test (keep bar 5: real hardware regression). Source: Toshiba audit 2026-11-25; PR #11 Uncharted DLC timeout.

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
Check: test (existing upload/drive/links integration tests). Source: existing transfer design and rewrite.

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
invalidate the epoch as specified in `specs/bgft.md`. Fresh fallback reads use exactly the same canonical health
semantics as the snapshot; they differ only in I/O cost.
Check: test (keep bar 1: external file format; keep bar 3: scan ordering). Test: `test/unit.c` ›
`test_bgft_info` and `test_bgft_scan`. Source: performance audit 2026-11-25; flatz BGFT format notes.


### P4N-14: nullfs ownership is explicit and fail-closed
Mount mutation is based on ownership evidence, not on the fact that a target is mounted. Current hold-folder
layers, proven legacy direct-drive layers, foreign layers and unreadable observations are distinct states;
foreign or unknown layers are never force-unmounted. Mount and detach postconditions are defined in
`specs/mount.md` and their decision tables are host-testable.
Check: test (security/data loss). Test: `test/unit.c` › `test_mountplan`. Source: mount ownership rewrite 2026-11-25.

## Toshiba reference profile (not a compatibility requirement)

The user's library disk observed on 2026-11-25: external USB 3.0 enclosure, 1.0 TB, exFAT, volume `TOSHIBA`,
about 329.5 GB free. The active library contained over 100 PKG metadata entries when including test residue;
one Black Ops II base pkg was actively downloading and incomplete, so no write or benchmark was performed.
Two tiny add-on pkgs were nonzero files whose headers declared package size zero; one corresponds to the
Uncharted add-on that timed out during the PR #11 hardware run. A `.ps4mount-prueba-fighterz` tree used about
7.4 GB of test residue. These facts motivate validation and cleanup audits, but the implementation must remain
drive-agnostic.

## Open questions requiring console proof

- Whether any safe interface can make BGFT ingest launch tasks without reboot.
- Exact sandbox visibility of host nullfs mounts for emulator/homebrew data on each GoldHEN/firmware pair.
- Whether generated PDB/PBM/PlayGo bytes differ across PS4 firmware families.
- Whether forced unmount of dead USB-backed nullfs layers is safe on every supported firmware.
- Why EA SPORTS FC 27 hit `CE-34224-5` in the PR #11 run after transfer activity; not enough evidence to
  blame the mount design.
