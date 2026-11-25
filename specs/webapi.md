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
was unreadable. The scan body uses its epoch snapshot, not a mutable global flag. The panel's
fresh scan may also carry a hard-invalidation intent: the owner clears the title cache and
left-out update markers only inside that epoch, after the system database is readable and
before the scan body uses them. The HTTP caller publishes the intent; it never clears those
owner data structures itself.
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
bounded to eight connected fds, the owner queue to four parsed requests, and owner-bound
`/api/check` bodies to 16 MiB in total; beyond those bounds a new connection is closed or
answered `503`, never allowed to consume unbounded memory. If no worker can be created,
`web_serve` falls back to the historical synchronous path rather than dropping the panel.

`/api/check` remains an owner route because its answer depends on library/configuration
state and uses one shared temporary path. Its declared body is nevertheless bounded, so a
worker reads that body completely before queueing the request. The allocated body, its byte
quota and the fd move as one owner intent. If the body arrives short the owner still produces
the historical "did not arrive whole" answer, but it never waits on the socket itself; if the
body quota cannot be reserved the request is answered `503` without entering the owner queue.

Only routes whose state is static or internally synchronized may complete in a worker in
this first matrix: the two HTML pages, `/api/log`, Drive status/actions, links
status/actions, `/api/upload`, `/api/uploads`, `/api/rescan`, `/api/nas/scan`,
`/api/nas/shares` and `/api/nas/reload`. `/api/rescan` is worker-safe only because its cache/marker invalidation
is part of the synchronized scan intent consumed later by the owner epoch. `/api/nas/reload`
keeps its synchronous BinLoader answer but is serialized with the automatic reload attempt;
the owner uses a non-blocking trylock and skips its retry while a worker holds that reload,
so protecting the shared payload-posting path cannot block the main loop.
The two NAS probe routes may connect to other machines but do not use the mounted backend:
scan uses fresh sockets, share listing creates its own backend context, their output arrays
are local, and the saved-login fallback is copied through the configuration mutex. SMB context
creation/destruction is serialized across the whole process because libsmb2 keeps a global
active-context list; ordinary operations then use per-context state. The probes may therefore
wait in workers rather than in the owner. `/api/stop` is also worker-safe: it publishes only
a one-way atomic stop intent, so the request is not stranded behind a long scan. `/api/check`,
games/icons/pics, storage, destinations, mounts, restore, configuration changes and
`/api/app/close` remain owner routes because they read or mutate library/mount/configuration
state or call process-control services of the system. Unknown routes are worker safe because
they produce only the fixed 404 response.

An owner route transfers both the connected fd and the parsed request object exactly once
to the owner queue. The main loop executes it through the same dispatcher, preserving the
response contract. A worker must adjust the body pointer when copying the request into a
queue node; it may never leave that pointer referring to its own stack. A pipe or equivalent
wake mechanism tells the main loop that owner work is pending, so responses do not wait for
the normal select timeout.

Owner responses are captured whole while they are produced, then their fd and bytes move
together to a bounded writer queue. A worker transmits every byte and closes the fd; the
owner never waits for a slow client socket. A response is never truncated by policy: if the
writer queue cannot accept it whole, the connection is closed instead. The capture boundary
is private to the web server and applies only to owner dispatch; worker dispatch writes to
its socket directly. Shutdown interrupts active connection reads/writes with `shutdown()`,
joins the bounded workers, and closes/drains queued fds without executing partially owned
requests or transmitting partial queued responses.

NAS probe output buffers are per-call; concurrent probes may run, bounded by the worker
count. Drive and links status may be produced concurrently. Their mutating actions are serialized
at the web boundary, preserving the single-panel-caller ordering those subsystems had before
workers; their internal queue state remains protected by their own mutexes, and queue-count
snapshots used for admission are read under those mutexes. A Drive library enumeration can
ask whether the NAS is active while a scan or FUSE client is changing that state. The answer
is an atomically published scalar mirror updated where the owned state changes; readers never
wait for network I/O or the FUSE lock. The adopted-mount refresh clock is claimed with
compare-and-swap so overlapping owner refreshes cannot pile control queries onto the mount.
Workers never start that control conversation as a side effect of reading `nas_active`; only
the main-loop owner calls the explicit refresh entry point.
Check: test through `test/http_test.c` (owner execution ordering, worker-only route
completion without owner pumping, owner response writer handoff and pre-read check bodies)
and `test/sdkcheck.sh`; read for shutdown ownership.
Source: panel/API architecture audit and WEBAPI-2 implementation design.
