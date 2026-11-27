# System-state snapshot and replace spec

Governs: which successful reads of `app.db` may become the catalog (`src/db.c`, `src/dbsnap.h`)
and how Lodestar replaces a file it is allowed to write (`src/durable.c`, `write_file` in `src/main.c`).
The interruption inventory and the hazards this spec does not close are in `docs/hazards/system-state.md`.
Nothing here is a console measurement.

## Rules

### SYS-1: a hot app.db journal does not prove a title disappeared
Lodestar only reads `app.db` (`mode=ro&nolock=1`). A successful read is published as the trusted
title list only when `db_snapshot_publish` allows it.

- The first successful read of a process is not published while `app.db-journal` exists.
- A later successful read that contains fewer title rows than the last published snapshot is not
  published while `app.db-journal` exists.
- When a read is withheld, the previous snapshot stays in memory, `db_ready` stays as it was, and
  neither `db_confirms_registered` nor `db_confirms_unregistered` succeeds for that read.
  `db_is_registered` still answers from the previous snapshot.
- A shrink after the journal file is gone is published, including an empty catalog.
- Growth, and a read with the same count, are published even if the journal exists. A torn read
  that drops one title and adds another can keep the same count; that case is not detected.
- Lodestar still never writes a system database, takes a lock on one, or deletes a journal.

Check: test (data-loss boundary: a false absence can commit `delete_pending_decision`). Tests:
`test/sysstate_test.c` (`test_snapshot`); `test/db_test.c` (journal held by a writer, then a
committed shrink once the journal is gone).
Source: host hazard hunt 2026-11-27, after the firmware 14.00 app.db rebuild. Not console-tested.

### SYS-2: a file Lodestar replaces is never left as a short prefix
`durable_replace` writes the new bytes to a temporary file in the same directory, `fsync`s it
(`EINVAL` and `ENOTSUP` are not fatal; any other `fsync` error is), and `rename`s it over the
destination. The destination is not opened with `O_TRUNC`. `write_file` in `src/main.c` is this
function, so param.sfo, license files, BGFT task files, markers and config all go through it.

- Failure before `rename` leaves the previous directory entry unchanged and removes the temporary file.
- A crash before `rename` can leave the temporary sibling (`<name>.tmp.<random>`). The path itself
  is still the previous file, or absent when there was none.
- An existing regular file keeps its permission bits. A new file is created with the requested mode
  exactly; the process umask is not applied.
- A symlink at the destination is replaced and not followed.
- Two paths are still two commits. A crash between `free<prefix>.rif` and `.idx`, or between
  `d0.pdb` and `d1.pdb`, can leave the pair on different generations. That gap is a follow-up,
  not this rule.

Check: test (crash window on files the system and BGFT read back). Test: `test/sysstate_test.c`
(`test_durable`).
Source: host hazard hunt 2026-11-27. `write_file` previously used `O_TRUNC`. Not console-tested.

## Non-goals

- Binding an `AppUnInstallMainExecute` result to the pending title. `DEL-6` still commits on a
  zero result. `docs/hazards/system-state.md` H1 tracks that gap.
- Rolling back appmeta, license and playgo when registration fails.
- Refusing to rewrite `/system_data/priv/appmeta/<TID>/param.sfo` while a nullfs hides a real `app.pkg`.
- Unmasking `/user/app` from `shutdown_daemon`, or keeping the console out of rest during install.
- Writing `app.db`, `appinfo.db`, `addcont.db`, or `av_content_bg.db`. Lodestar does not read
  `av_content_bg.db`. It only stats `addcont.db` to see if the system is busy.
