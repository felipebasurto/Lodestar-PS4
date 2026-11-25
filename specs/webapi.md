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
