# Delete safety spec

Governs: console deletion flows, removal of generated holds, and deletion of library files.
Sources: AI-WORKFLOW.md rule 2; PR #11 compatibility work; testectomy audit 2026-11-25.

## Purpose

The console's delete flows must be able to remove what the system owns, and PS4Mount may remove only the
files it explicitly owns. A mistake in this area is not a glitch; it is user data loss or a library that can
never be freed.

## Rules

### DEL-1: a real library delete removes only `<root>/ps4/<TID>` byte-exactly
The delete path must be exactly the library folder of the same valid title id under one scanned root.
A trailing slash, case variation, in-place pkg path, foreign root, invalid title id, or parent path is refused.
Check: test (keep bar 2: data loss). Test: `test/unit.c` › `test_delete_target`. Source: AI-WORKFLOW.md rule 2.

### DEL-2: in-place user packages are never deleted by the console-delete path
When a game is used where the user put it, console deletion removes registration/holds only and leaves the
pkg in place. The scan must not immediately add it back unless the user asks.
Check: read (the state machine remains in `main.c` pending console validation). Source: current behavior.

### DEL-3: the delete window opens on confirmed PS4 delete screens and stays open through their dialogs
The classifier recognizes the two PS4 delete flows: home-screen Options > Delete and Settings > Storage.
Menus/confirmation/deletion scenes keep it open; unrelated scenes close it. Unconfirmed PS5-only names are
legacy compatibility inputs, not evidence of PS4 support.
Check: test (keep bars 2 and 5: data loss and past regressions). Test: `test/unit.c` › `test_delete_scene`.
Source: AI-WORKFLOW.md rule 2.

### DEL-4: a delete event outside an open delete window is not a user delete
System/PS4Mount uninstall work outside the window must not trigger deletion of library files. Content ids used
in paths must be plain validated characters.
Check: read until the state machine is extracted; then test. Source: regression documented in `src/main.c`.

## Non-goals

- Interpreting a failed read of system databases as proof that a title is absent.
- Deleting foreign files that happen to share a folder with PS4Mount files.
- Treating unmount failure as successful deletion.

## Open questions

- Which exact scene names differ across firmware families and languages. Hardware validation is required
  before claiming broader delete-flow compatibility.
