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

### WEBAPI-6: bounded network workers never execute owner routes
At most two persistent network workers parse accepted connections. The accept queue is
bounded to eight connected fds and the owner queue to four parsed requests; beyond those
bounds a new connection is closed or answered `503`, never allowed to consume unbounded
memory. If no worker can be created, `web_serve` falls back to the historical synchronous
path rather than dropping the panel.

Only routes whose state is static or internally synchronized may complete in a worker in
this first matrix: the two HTML pages, `/api/log`, Drive status/actions, links
status/actions, `/api/upload` and `/api/uploads`. `/api/check`, games/icons/pics, storage,
destinations, mounts, restore, configuration, NAS and process control remain owner routes
because they read or mutate library/mount/configuration state. Unknown routes are worker
safe because they produce only the fixed 404 response.

An owner route transfers both the connected fd and the parsed request object exactly once
to the owner queue. The main loop executes it through the same dispatcher, preserving the
response contract. A worker must adjust the body pointer when copying the request into a
queue node; it may never leave that pointer referring to its own stack. A pipe or equivalent
wake mechanism tells the main loop that owner work is pending, so responses do not wait for
the normal select timeout. Shutdown interrupts active connection reads with `shutdown()`,
joins the bounded workers, and closes/drains queued fds without executing partially owned
requests.

Drive and links status may be produced concurrently. Their mutating actions are serialized
at the web boundary, preserving the single-panel-caller ordering those subsystems had before
workers; their internal queue state remains protected by their own mutexes, and queue-count
snapshots used for admission are read under those mutexes. A Drive library enumeration can
ask whether the NAS is active while a scan or FUSE client is changing that state. The answer
is an atomically published scalar mirror updated where the owned state changes; readers never
wait for network I/O or the FUSE lock. The adopted-mount refresh clock is claimed with
compare-and-swap so overlapping owner refreshes cannot pile control queries onto the mount.
Workers never start that control conversation as a side effect of reading `nas_active`; only
the main-loop owner calls the explicit refresh entry point.
Check: test through `test/http_test.c` (owner execution ordering and worker-only route
completion without owner pumping) and `test/sdkcheck.sh`; read for shutdown ownership.
Source: panel/API architecture audit and WEBAPI-2 implementation design.
