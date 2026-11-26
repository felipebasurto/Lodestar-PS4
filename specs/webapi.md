# Web/API ownership spec

Governs: HTTP parsing and application dispatch in `src/web.c`, plus the ownership contract
for moving panel work out of the main loop. Source: panel/API architecture audit 2026-11-25.

## Rules

### WEBAPI-1: network parsing and application decisions are separate boundaries
Reading sockets, HTTP framing, Content-Length validation and bounded body acquisition are the
network boundary. Route decisions and calls into application state are the application boundary.
A request object must own the bytes passed between them; application dispatch must not read the
socket again except for explicitly assigned streaming routes (`/api/upload` and links).
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
bounded to eight connected fds, the owner queue to four parsed requests, and
`/api/check` bodies to 16 MiB in total; beyond those bounds a new connection is closed or
answered `503`, never allowed to consume unbounded memory. If no worker can be created,
`web_serve` falls back to the historical synchronous path rather than dropping the panel.

`/api/check` is a worker route. Its declared body is bounded, so the worker reads that body
completely before dispatching the request; the owner never waits on the socket. If the body
arrives short the route still produces the historical "did not arrive whole" answer without
calling the application check; if the body quota cannot be reserved the request is answered
`503`. Validation writes to a per-request temporary name, never the historical shared
`check.part`, and reads configuration through a snapshot. In-place game presence comes from
an immutable catalog epoch snapshot published only when a catalog epoch is complete; a
worker never takes the catalog lock that a scan may hold while pumping events.

Only routes whose state is static or internally synchronized may complete in a worker in
this first matrix: the two HTML pages, `/api/agent`, `/api/status`, `/api/log`, Drive status/actions, links
status/actions, `/api/upload`, `/api/uploads`, `/api/rescan`, `/api/dests`, `/api/storage`,
GET `/api/games`, icons/pics, GET `/api/config`, POST `/api/check`, `/api/nas/scan`, `/api/nas/shares` and `/api/nas/reload`. `/api/agent` is static. `/api/status` copies only
atomics, mutex-protected snapshots and web queue counters; it never enters the catalog or
title-cache locks and never enumerates the library. `/api/rescan` is worker-safe only because its cache/marker invalidation
is part of the synchronized scan intent consumed later by the owner epoch. `/api/nas/reload`
keeps its synchronous BinLoader answer but is serialized with the automatic reload attempt;
the owner uses a non-blocking trylock and skips its retry while a worker holds that reload,
so protecting the shared payload-posting path cannot block the main loop.
The two NAS probe routes may connect to other machines but do not use the mounted backend:
scan uses fresh sockets, share listing creates its own backend context, their output arrays
are local, and the saved-login fallback is copied through the configuration mutex. SMB context
creation/destruction is serialized across the whole process because libsmb2 keeps a global
active-context list; ordinary operations then use per-context state. The probes may therefore
wait in workers rather than in the owner. GET `/api/config` reads a whole-config snapshot
copied under the configuration mutex and a NAS status snapshot copied under a small snapshot
mutex; that status copy never starts an adopted-mount control conversation. `/api/stop` is
also worker-safe: it publishes only a one-way atomic stop intent, so the request is not
stranded behind a long scan. `/api/storage` copies only the managed-game identities it
needs under a short mutex, then measures filesystem trees outside that mutex through a
bounded iterative traversal. Mounts, restore,
configuration changes and `/api/app/close` remain owner routes because they read or mutate library/mount/configuration
state or call process-control services of the system. Unknown routes are worker safe because
they produce only the fixed 404 response.

Icons and pics are worker-safe because their fast path reads `/user/appmeta` through the
same narrow file mutex used when registration writes those extracted entries, so a worker
cannot observe a partially written image. Their fallback uses the same request configuration
snapshot plus catalog epoch snapshots to find the game/update pkg paths. They never enter
`g_cat_lock` or the title cache; opening the pkg entries in the worker preserves the
historical on-demand fallback without blocking the owner.

`/api/games` is worker-safe only because every library owner it reads has an immutable or
copied boundary. In-place rows come from a refcounted catalog epoch snapshot carrying the
grouped game/update/add-on paths and content identities needed for both fingerprints and
cache-miss pkg opening; workers never take `g_cat_lock`. The scan owner still uses its live
catalog. Title-cache lookup preserves on-demand pkg reading after a cache miss, and the
result is deep-copied under the title-cache mutex, including add-on descriptors, so neither
a cache entry nor its add-on pool pointer escapes into the response. Configuration, setup,
restart membership, running titles, notifications, activity and response buffers all use
their synchronized snapshot, atomic or per-call forms. Publishing only title IDs or using a
cached fingerprint without the cache-miss path would change freshness and is not a valid
migration.

An owner route transfers both the connected fd and the parsed request object exactly once
to the owner queue. The main loop executes it through the same dispatcher, preserving the
response contract. A worker must adjust the body pointer when copying the request into a
queue node; it may never leave that pointer referring to its own stack. A pipe or equivalent
wake mechanism tells the main loop that owner work is pending, so responses do not wait for
the normal select timeout. While `/user/app` is masked for registration, accepted owner work
waits too: a queued mount, unmount, restore or configuration action must not inspect or mutate
through the mask, and becomes dispatchable again only after the real `/user/app` tree is
restored.

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

### WEBAPI-7: the agent manifest and status endpoints are read-only additive contracts
The product version exposed through the existing API (`version` in responses such as
`GET /api/games`) is `Lodestar 2.0.2`. The internal state and log paths remain under
`/data/ps4mount` for compatibility; this rule does not migrate console state.

`GET /api/agent` returns a machine-readable JSON manifest with `product` (`Lodestar`),
`version` (`Lodestar 2.0.2`), `api_version` (`1`), the panel scheme/port, the write-header
model (including that the server checks for the custom header's presence), an endpoint list
in which every entry has `method`, `path`, `mutating` and `summary`, and safety rails.
In that list `mutating` means intentionally starting or changing application/system state or
work; temporary validation files and read-only network probes are not mutations. The manifest
is static apart from those versioned constants, uses no application locks and is worker-safe.

`GET /api/status` returns a lightweight JSON snapshot with `product`, `version`,
`api_version`, `setup`, `activity`, `queues`, at most five recent `notices`, and key
`endpoints`. `activity` has the same fields and meaning as the `activity` object of
`GET /api/games`, including `dlc_pending`, but `/api/status` must not enumerate the library
or copy the game list. `queues.web_owner_pending`, `queues.web_write_pending` and
`queues.web_write_active` are read through the web server's synchronized counters. Status is
observational: it does not touch the first-setup presence timestamp, start work or mutate
application state. Both endpoints are GET-only; unknown methods keep the normal method
handling.

Check: test through the existing real-socket HTTP harness (`test/http_test.c`); read for
snapshot locking and for the absence of catalog/library enumeration in `/api/status`.
Source: Lodestar agent control-plane design.

### WEBAPI-8: `X-Lodestar` is the canonical write header and `X-PS4Mount` its legacy alias
Requests that write files or otherwise change state keep the existing threat model: POSTs
(and the network-probing NAS scan) must carry the panel header and come from an allowed
local/non-public IPv4 peer. `X-Lodestar: 1` is accepted everywhere the panel header is
required. `X-PS4Mount: 1` remains accepted as a legacy alias for existing panels, extensions
and scripts. Archive passwords may be sent as `X-Lodestar-Password`; the legacy
`X-PS4Mount-Password` remains accepted as its alias. This rule does not add a token,
change a port, or weaken the network boundary.
Check: test through `test/http_test.c`; read for header-block-only parsing in `may_write`.
Source: Lodestar compatibility contract.

### WEBAPI-9: `/api/log` metadata is additive
`GET /api/log` keeps returning the JSON string field `log` with the same tail of
`/data/ps4mount/log.txt` as before (at most 16 KiB, with the same escaping). It also returns
`bytes` (the number of raw log bytes read into that tail before JSON escaping/filtering), `truncated`
(true when the file had bytes before that tail), and `path` (`/data/ps4mount/log.txt`).
Clients that only read `log` must keep working. Byte ranges are not part of this contract.
Check: test through `test/http_test.c`; read for bounded file reading.
Source: Lodestar agent observability contract.
