# BGFT task-state spec

Governs: `src/bgftscan.c`, `src/bgftinfo.h` and BGFT task decisions in `src/main.c`.
Sources: strict scan-snapshot design in commits 63776ac/28f5a65 and the registration-cost audit.

## Rules

### BGFT-1: a scan epoch may answer absence, but positive mutation evidence is fresh
Within one serialized root scan/registration epoch, a task absent from the snapshot is treated
as absent. A task present in the snapshot is re-read directly before mutation decisions use it.
This keeps new-title registration from rescanning every task for every game without turning stale
positive evidence into truth.
Check: test (ordering, external system state and large input space). Tests: `test/unit.c` ›
`test_bgft_scan`; compile boundary `test/sdkcheck.sh`.
Source: rewrite BGFT audit 2026-11-25.

### BGFT-2: successful writes update the epoch; uncertain mutations invalidate it
After PS4Mount writes both PDB files successfully, or proves both already byte-identical during
the same registration attempt, the snapshot is updated from the exact canonical bytes. If either
write fails, a task is removed, or freshness cannot be proven, the snapshot is invalidated and
callers fall back to direct reads. External creation races cannot be closed without a BGFT-wide
lock; the epoch does not claim to close them.
Check: test. Test: `test/unit.c` › `test_bgft_scan` for update semantics; integration by reading.
Source: rewrite BGFT audit 2026-11-25.

### BGFT-2A: task-id occupancy is separate from task identity
Every numeric directory in the snapshotted task-id range is occupancy evidence, including the
even install-task id below a game id, an unreadable task and a task whose PDB has no usable
title identity. A malformed task must not be treated as an available id merely because its title
could not be parsed.
Check: test. Test: `test/unit.c` › `test_bgft_scan` occupancy assertions.
Source: first-principles audit of the registration epoch, 2026-11-25.

### BGFT-4: allocation never reuses an id taken within the payload run
An id returned by the system installer for any install PS4Mount starts is occupied immediately,
even before its task directory exists and even if starting or installing it later fails. An id
whose game task PS4Mount removes is occupied for the rest of the payload run too, because BGFT
does not forget task state within a boot. New game-task allocation treats this boot-scoped taken
set as occupancy in addition to the scan epoch or direct directory evidence; otherwise a system
install task created inside the scan epoch can be overwritten by a game task.
Check: test (ordering and external system state). Test: `test/unit.c` › `test_bgft_scan` allocation
assertions; compile boundary `test/sdkcheck.sh`.
Source: firmware 14.00 collision between an add-on install task and a game task, 2026-11-26.

### BGFT-3: malformed tasks remain unknown, never healthy
Canonical records with wrong widths, conflicting identity evidence, unreadable tasks or malformed
PDB frames are not proof of a healthy registration. Legacy identity may locate a task for rewrite,
but launch health requires canonical metadata.
Check: test. Tests: `test/unit.c` › `test_bgft_info`, `test_bgft_scan`.
Source: commit 28f5a65 semantics.
