# Mount ownership spec

Governs: `src/mountplan.h`, and in `src/main.c` the observation, mounting and detaching of
`/user/app/<TID>`, `/user/patch/<TID>` and `/user/addcont/<TID>` (`mount_dir`,
`detach_target`, `unmount_title`, `title_detached`).
Sources: AI-WORKFLOW.md rule 2, the 0.3.2 hold-folder migration, and the rest-mode fix in
commit `19903b2` (current hold folders stay mounted while a drive is away; only legacy direct
drive mounts are released).

## Purpose

A mount point is shared kernel state. "Something is mounted" is not ownership evidence: another
payload, an older PS4Mount or a user action may have put it there. Unmounting a foreign layer can
break another component or expose the wrong folder; leaving it in place is recoverable. Mount
decisions therefore fail closed: PS4Mount removes only layers it can prove are its own, and a
foreign or unknown layer blocks the operation.

Nothing in this area is hardware-validated yet. These rules define the intended kernel boundary;
the console still has to prove the forced-unmount and drive-reconnection behavior.

## Rules

### MNT-1: ownership is evidence, never the target path alone
A nullfs layer is current-owned only when its source is byte-exactly the hold folder the caller
intends to mount (`/system_data/ps4mount/app/<TID>`, `patch/<TID>` or `addcont/<TID>`). For app
and patch targets only, a layer is legacy-owned when its source is under `/mnt/` and the first
line of `/data/ps4mount/<TID>.src` is byte-exactly that source; this is the direct drive-folder
mount left by versions before the hold-folder migration. Anything else is foreign. Missing or
unreadable ownership evidence makes the layer foreign for decision purposes, never owned.
Check: test (security/data loss). Test: `test/unit.c` › `test_mountplan`.

### MNT-2: routine mounting removes only owned layers
When a target has to be mounted, an exact current-owned top layer is already the postcondition.
Otherwise PS4Mount may unmount the top layer only while the live top is owned and every visible
lower layer is owned too. If a foreign lower layer already makes replacement impossible, the
operation refuses without disturbing the owned state that is currently in place. If the target is
inaccessible through `statfs` (a dead or inconsistent layer), it may unmount only when every
layer visible in the mount table is owned. A foreign or unknown layer makes the mount fail; it is
never unmounted to make room.
Check: test for the decision table (`test_mountplan`); read for the syscall integration.

### MNT-3: detaching for delete or uninstall is owned-only even under force
`MNT_FORCE` changes how an owned layer is removed, never which layer may be removed. A forced
detach may use force on an owned dead/inaccessible layer, but foreign and unknown layers still
block. Delete and uninstall paths must treat a remaining layer as a failed postcondition and must
not let the system delete beneath it.
Check: test for the decision table (`test_mountplan`); read for syscall integration. Hardware
validation remains required before release.

### MNT-4: a successful mount or detach ends in an auditable postcondition
A successful mount means the live top layer is nullfs whose source is byte-exactly the intended
hold folder. A successful detach means the mount table contains no layer at all on the target
(not merely no readable top layer). If the observation needed for those postconditions cannot be
made, the operation reports failure rather than success.
Check: test for the decision table (`test_mountplan`); read for the syscall integration.

### MNT-5: rest mode does not make current hold mounts stale
Current-owned hold folders contain links and deliberately stay mounted when the drive disappears;
their links resolve again when the drive returns. Only legacy direct `/mnt` mounts are candidates
for stale release, and only when they satisfy MNT-1's legacy ownership evidence.
Check: read. Source: commit `19903b2` and its rest-mode regression note.

### MNT-6: emulator mounts keep their separate exact contract
Emulator mounts under `/data` are not game-target layers and continue to use the exact source/target
pairing of `specs/emulators.md`. This spec does not widen or narrow that contract.
Check: read.

### MNT-7: cached mount-table observations may only prove no-work states
Reading the full mount table once per title per scan is prohibited: the table can hold over
a thousand entries. A bounded one-second snapshot may answer routine observations, and local
mount/unmount mutations invalidate it immediately. The snapshot may prove only no-work states
(`DONE` or `ALREADY`) in routine scan maintenance. Before any `unmount(2)` or `nmount(2)`
decision is executed, the target is observed again from a fresh table and the same pure
planner decision is recomputed; if the fresh facts no longer request that mutation, no
mutation occurs. Detaches that precede delete, uninstall or installer operations do not use
the snapshot at all. Destructive postconditions such as `layers_on_failclosed` still read
fresh rather than using the snapshot.
Check: read (kernel mount-table integration; pure ownership decisions remain covered by
`test/unit.c` › `test_mountplan`, and compile boundary by `test/sdkcheck.sh`).
Source: scan performance audit after P4N-15.

## Non-goals

- Unmounting mounts made by other payloads or by the user.
- Migrating a legacy direct mount that lacks enough `.src` evidence. It blocks until reboot or
  manual cleanup; that is deliberate fail-closed behavior.
- Proving whether forced removal of dead USB-backed nullfs layers is safe on every firmware. That
  remains hardware evidence to collect.

## Open questions and limits

- There is no compare-and-unmount syscall: a foreign layer can theoretically be mounted after the
  ownership observation and before `unmount(2)`. PS4Mount cannot close that kernel race from userland;
  it can only make every decision from the freshest observation it has and never treat force as ownership.
  Normal operation does not share these target paths with another component.
- Does `getfsstat` expose enough ordering information on every supported firmware to identify the
  top dead layer when several layers are stacked? Until proven, the conservative rule is all-owned
  or refuse.
- Can `/user/app` masking be made fully ownership-aware without changing ShellCore registration
  timing? The staging mount itself is current-owned, but the stacked namespace needs console
  evidence before further changes.
