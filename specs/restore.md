# Restore safety spec

Governs: `src/restore.h`, and in `src/main.c` the restore pipeline (`restore_unregister`,
`restore_files`, `restore_pkg`, `restore_clear_state`, `run_restore`).
Sources: delete/restore audit 2026-11-25, AI-WORKFLOW.md rules 2 and 5.

## Purpose

Restore moves packages PS4Mount organized under `<root>/ps4/<TID>` back to their recorded original
locations. It crosses three boundaries that must stay ordered: the running game, the system's
uninstall/registration state, and user files on a writable drive. A wrong ordering can strand a game
without ownership evidence or move user data under the wrong path.

## Rules

### REST-1: no restore mutation starts while a game is open
The batch closes open games first. If one does not close, no package is unregistered, moved, or
state-cleaned.
Check: read. Source: existing restore flow; hardware timing still requires console validation.

### REST-2: detach and system removal are the commit boundary for each title
`restore_files` may run only after `restore_unregister` succeeds for that same title. The unregister
step must first prove app, patch and add-on targets detached; a title that cannot detach is kept even
when it is installed internally and no system uninstall is needed. For titles not installed internally,
the removal admission/uninstall rules of `specs/delete.md` must also succeed. If unregistering is refused
or fails, that title's files and ownership state stay as they were; other titles in the batch do not
affect it.
Check: test (data-loss ordering). Test: `test/unit.c` › `test_restore_plan`.
Source: restore audit 2026-11-25.

### REST-3: state cleanup is per-title, after the commit boundary, and only without move failures
`restore_clear_state` may run only for a title whose unregister succeeded and whose package moves did not
fail. A title kept in the library keeps its `.src`, `.removed`, retry and BGFT state. A title whose system
entry was removed but whose files could not all move keeps the `.removed` suppression marker so the next scan
cannot silently re-register the leftovers; the user can add it again explicitly after inspecting the log.
Cleanup of one restored title never clears state for another title.
Check: test (data-loss ordering). Test: `test/unit.c` › `test_restore_plan`.
Source: restore audit 2026-11-25.

### REST-4: original paths are untrusted external input
A path read from `origin.txt` must be a non-empty relative path using `/`, with no backslash, empty
component, `.` component, or `..` component. Anything else is refused before joining it under the drive
root. The fallback generated from pkg metadata is cleaned before the same validation.
Check: test (large input space; path traversal/data loss). Test: `test/unit.c` › `test_restore_plan`.
Source: restore audit 2026-11-25.

### REST-5: restore never knowingly overwrites a user file
The destination is allocated with `origin_free_path`; if no free name or parent directory can be used,
the move fails and the source package remains in the organized folder. Rename is not a proof of atomic
no-replace on every filesystem, so a race with another writer remains a known limitation.
Check: test for free-name allocation (`test/unit.c` › `test_origin`); read for the rename boundary.
Source: existing origin flow.

### REST-6: only empty organized folders are removed
Restore removes generated metadata/holds, but game folders and add-on folders are removed with `rmdir`
only when empty. Foreign files left in those folders prevent removal and are never deleted.
Check: read. Source: AI-WORKFLOW.md rule 2.

## Non-goals

- Restoring system-installed titles by uninstalling the internal copy.
- Rolling back a successful system uninstall because a later file move failed.
- Treating a partial file move as proof that all files were restored.

## Open questions

- Whether the panel should surface partial-move failures as a per-title repair action instead of requiring
  the user to read the log and explicitly add/retry. Hardware behavior and user experience need evidence
  before changing that contract.
