# Google Drive transfer spec

Governs: `src/drive.c` and its host integration test.
Sources: existing transfer design; testectomy audit 2026-11-25.

## Rules

### DRIVE-1: upload progress describes the whole game, not one file
While a game upload is sending, status exposes the sending phase and the byte total of its base package,
update and add-ons together. Completion may replace progress with the final result.
Check: test (keep bar 6: public panel contract). Test: `test/drive_test.c` › uploading a game in chunks.
Source: existing panel contract.

### DRIVE-2: interrupted transfers resume without corrupting bytes
A cut upload or download continues from bytes already verified; a damaged download is not kept as good.
Check: test (keep bars 2 and 5). Test: `test/drive_test.c`. Source: existing implementation.

### DRIVE-3: a game's folder appears only when all its packages are whole
A scan must never adopt a half-downloaded game.
Check: test (keep bar 2: data integrity). Test: `test/drive_test.c`. Source: existing implementation.

### DRIVE-4: panel actions read account state through the subsystem mutex
Public Drive actions may be called while the Drive worker is signing in, disconnecting or
refreshing credentials. `configured`/`connected` readiness checks and forget/disconnect
decisions must read that state through `g_mu` (or an equally narrow snapshot), never through
unsynchronized shared words. Holding the mutex is limited to copying scalars; network and file
work stays outside it.
Check: test through the existing concurrent Drive integration harness (`test/drive_test.c`);
read for the narrow readiness snapshot. Source: web worker ownership audit.
