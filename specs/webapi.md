# Web/API ownership spec

Governs: HTTP parsing and application dispatch in `src/web.c`, plus the ownership contract
for moving panel work out of the main loop. Source: panel/API architecture audit 2026-11-25.

## Rules

### WEBAPI-1: network parsing and application decisions are separate boundaries
Reading sockets, HTTP framing, Content-Length validation and bounded body acquisition are the
network boundary. Route decisions and calls into application state are the application boundary.
A request object must own the bytes passed between them; application dispatch must not read the
socket again except for explicitly assigned streaming routes (`/api/check`, `/api/upload`, links).
Check: test through the existing real-socket HTTP harness (`test/http_test.c`); read for the
ownership split. Source: panel/API architecture audit.

### WEBAPI-2: the main loop remains the owner of game, mount, registration and configuration state
A future web worker may parse requests concurrently, but it must not mutate those state machines
directly. Mutating routes are intents executed by the owner; worker threads may wait for the
result so the external API response compatibility is preserved. Read-only state must likewise
come from an explicit snapshot or owner execution, not unsynchronized shared access.
Check: read until the worker/queue implementation exists; concurrency behavior must then be
covered by tests. Source: panel/API architecture audit.

### WEBAPI-3: streaming fd ownership is explicit
`/api/upload` may transfer a connected fd to the upload subsystem exactly once. After a
successful transfer the web layer must not answer or close it. Failed transfer leaves ownership
with the web layer, which answers and closes. Other routes close their own connection after the
response.
Check: test through `test/http_test.c` and `test/upload_test.c`; read for ownership.
Source: existing upload behavior, made explicit.

### WEBAPI-4: scan requests are ordered intents, not shared flags
Upload, Drive and links threads may ask for a scan, and the panel may ask for a fresh manual
scan. Those requests cross the owner boundary through one synchronized scan-request machine.
A request that arrives while a scan is running must not be overwritten by that scan's normal
end-of-scan scheduling; it causes another scan. A manual request is consumed by one scan epoch
and is kept for the next scheduled epoch when the scan never ran because the system database
was unreadable. The scan body uses its epoch snapshot, not a mutable global flag.
Check: test for the ordering machine (`test/unit.c` › `test_scanreq`); read for mutex use in
`main.c`. Source: existing cross-thread `app_rescan()` callers, made explicit.

### WEBAPI-5: transfer threads see configuration through narrow synchronized accessors
The main loop may save settings while Drive, links or upload completion threads are active.
Those threads must not read `config_t` or the active language through shared words directly:
configuration fields reachable from their host callbacks pass through small mutex-protected
accessors, whole-config publication happens under the same mutex, and the active language is
published/loaded atomically. Main-loop-only users may keep reading the owner state directly.
Check: test for the language boundary (`test/unit.c` › `test_lang`); read for the narrow
configuration accessors and for no direct `g_cfg` use in transfer-thread callback paths.
Source: existing Drive/links host callback audit.
