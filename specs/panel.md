# Library panel

### PANEL-1: the library is one column of games

The web panel and the PS4 app present the library as a single column, one game per row, in title
order. A row shows small artwork, the title, and the state only when it differs from ready to play.
The chosen game's row grows in place to show larger artwork and its details: no side inspector or
separate shelf. In the PS4 app that row also shows what can be done with the game; in the web panel
choosing it opens its details, which hold those actions. Keyboard/controller focus and the current section
survive library refreshes. Moving focus or opening a game's details makes no write request.
Check: test for focus, details requests and filtering (`test/web/panel.test.js`), read for layout.
Source: user (2026-11-27): the header-and-grid layout copied the project Lodestar started from and
was awkward with a controller; they chose the one-column prototype. Replaces the artwork grid.

### PANEL-2: every action in the console app is one button away

In the PS4 app the controller moves along one axis. Up and Down walk the library, Left and Right
jump to the previous or next initial, L1 and R1 change section (needs attention, all, running).
Holding L2 opens a wheel with the app-wide actions (scan, send packages, transfers, settings,
search by name, exit); the stick or the arrows point and releasing L2 chooses. Releasing without
pointing, or Circle, closes it and does nothing. There is no menu to travel to. Circle closes the
wheel, the current page or the search before it exits the app. The web panels keep their own
keyboard route and Back handling.
Check: test for the web keyboard route and fallback (`test/web/panel.test.js`), read for gamepad input.
Source: user request for TV and controller operation; user (2026-11-27): a new way to move and act.

### PANEL-3: state that needs attention stands out

Ready games carry no label. Games that wait for a restart, are running, or whose drive or copy is a
problem say so in colour. The section for games that need attention holds those waiting for a
restart or to be added, with a disconnected drive, with another active copy, or in an unknown state.
The modern web panel keeps the search field, the state/platform filters and the drives' free space
above the list, the frequent actions (scan, send packages, import from URL, notifications) in the
header, and the rest (settings, remove several, log, stop) in one menu. Back closes an open menu
before leaving the PS4 panel, and in the PS4 browser Up/Down move out of the search field so it
never traps the controller.
Check: test for reaching details, filtering and Back (`test/web/panel.test.js`), read for hierarchy.
Source: user (2026-11-26): a clean, well organised interface; user (2026-11-27): lighter.

### PANEL-9: the same words everywhere, and every notice says what to do

The web panels and the PS4 app give each game state the same name. Opening a game that waits for a
restart, whose drive is away, that has another active copy, that is still to be added or that is
installed on the console says what that means and what to do. While any game waits for a restart,
both panels' libraries say so at the top: restart the console (rest mode does not count) and have
Lodestar loaded afterwards. While the system database is flagged as damaged (`db_corrupt`), both
web panels show an explicit top-level warning telling the user not to continue registering and to
restart/recover before continuing (`REG-7`). The console's notification for it asks to load Lodestar again unless it
starts with GoldHEN. A warning says what happened and, when the user can do something, what.
Check: test for the web names and restart notices (`test/web/panel.test.js`), read for the app and
the notifications.
Source: user (2026-11-27): do what is best for the user with the state names, warnings and restart
notices.

### PANEL-4: write requests come only from local network addresses

The panel has no password. A write request must carry the panel header and come from loopback,
a private LAN range, link-local, carrier-grade NAT space, or a local socket. Public IPv4 addresses
are refused before any file or settings write.
Check: test (keep bar 2: security boundary). Test: `test/unit.c` › `test_web_net`.
Source: existing panel threat model.

### PANEL-5: the PS4 tile opens a native controller interface

The standalone PS4 app uses VideoOut and Pad. It renders the library (PANEL-1), a game's details
and the commonly used settings in the Lodestar identity (PANEL-7), with a controller legend. It can
also queue one download link at a time and show download progress and recent jobs. It reads data
and submits actions exclusively through the same loopback HTTP API used by the web panel. Network
requests and artwork decoding run off the input/render loop. Library refreshes preserve selection by
title ID. Adding, removing and restoring use the existing API and are confirmed by holding the
button (PANEL-8). Cross adds or removes, Square restores the original files, Triangle opens the
details (title ID, version, add-ons, firmware, size, location). The mobile/desktop HTML panel
remains the place for local file uploads, bulk download management and advanced administration.
At phone widths, search spans the content, the header controls can wrap, and the connection
indicator stays visible. The legacy HTML URL and close route remain compatible with older clients.
Check: test for the native client's response parsing and HTTP boundary; read for render,
focus, process ownership and route use. VideoOut, Pad and the system keyboard require a
real PS4 smoke test before release.
Source: user approval of the native PS4 requirement (2026-11-26); user chose the one-column
prototype (2026-11-27).

### PANEL-6: package and URL transfer copy describes the user's files

In English and Spanish, the public interface describes sending packages and importing
files from a URL the user supplies. A package rejected for its format reports that format
as unsupported. The compatibility check and transfer behavior follow their existing rules.
Check: read for copy and technical accuracy; the existing panel and upload tests cover
the transfer controls and the reason shown when a package cannot be used.
Source: repository owner's public copy request (2026-11-26).

### PANEL-7: one quiet identity, drawn from original art

The web panels, the PS4 app and the browser extension share one light look: dark neutral grounds,
orange for whatever is chosen, condensed type (Barlow Condensed, SIL OFL) written in sentence case,
bold only for what is focused or titled, few boxes and lines. Capitals are the wordmark's. The mark
is flat: a white flying wing under an orange four-point star it steers by (the Lodestar of Black Ops
II was a drone, and a lodestar is the star one steers by). The wordmark is outlined from Michroma
(SIL OFL) in one colour. Nothing is rendered in 3D or textured, and the console's launch picture is
the lockup on a dark ground with one soft light. `assets/render_icons.py` is the source of the mark, wordmark and
pictures. No game artwork, logo, typeface or name of its publisher appears in the product.
The panel pages stay self-contained: `build.sh` inlines their font.
Check: read.
Source: user direction (2026-11-27): redo the previous identity, heavily inspired by Black Ops II;
then make it lighter. User choice of the flat "wing under its star" mark, after rejecting a 3D one
(2026-11-27).

### PANEL-8: a hold acts only on what was under the button the whole time

In the PS4 app, adding, removing and restoring happen after the button is held for 700 ms. Releasing
earlier does nothing. The hold is dropped if the focused game changes (for example after a library
refresh) or the action stops being available for it, and one press performs at most one action.
Check: test (data loss: restore moves files; ordering: refreshes arrive mid-hold).
Test: `test/panel_lane_test.c`.
Source: user chose hold-to-confirm in the one-column prototype (2026-11-27).

### PANEL-10: the native PS4 app exits only through SystemService

Every exit path of `LODS00001` — normal close (Circle on the library or Exit on the L2 wheel) and
early initialization failures — tears down initialized resources in order (wait bounded time for any
pending VideoOut flip, close an open IME dialog, cooperatively cancel and join the worker thread,
unregister and close VideoOut, unmap and release direct memory, close Pad, free caches and fonts,
flush the app log) and terminates via `sceSystemServiceLoadExec("exit", NULL)`. The app never returns
from `main`, never calls `exit`, `_exit`, `_Exit` or `abort`, and never detaches its worker thread.
If `sceSystemServiceLoadExec` returns, it logs the status, requests navigation home, and parks in an
idle sleep loop rather than returning to libc CRT teardown (which invokes `libkernel` `_exit` and
triggers `SIGSYS` / `ExceptionCode 12`).
Check: test (policy check in `test/policy.sh` for exit path, no `return` in `main`, no `exit`/`abort`,
and no `pthread_detach`; host worker/shutdown tests in `test/panel_app_test.c`; the whole `main.c`
run against fake system libraries in `test/panel_lane_test.c`, which counts the balanced opens and closes
and the `sceSystemServiceLoadExec` call). Real console exit still requires a PS4 smoke test.
Source: real PS4 rc.3 crash report (`ExceptionCode 12` / `SIGSYS` in `libkernel`), OpenOrbis v0.5.4
`crt1.o`/`libc.a` inspection, and upstream `PS4Mount-PS4` termination path (2026-11-27).

### PANEL-11: the native PS4 app keeps a bounded, redacted diagnostic log

The PS4 app records init, high-level input, L2 wheel actions, holds, API task lifecycle, errors and
exit to a bounded rotated log (`/data/lodestar/panel-app.log` with one `.old` backup) and an
in-memory ring buffer so failures remain recoverable even if file I/O fails. Full download URLs and
passwords are never written to the log.
Check: test (privacy/redaction boundary and rotation cap in `test/panel_app_test.c`).
Source: PS4 rc.3 crash investigation (2026-11-27).

### PANEL-12: rendering composites in CPU RAM and updates only damaged double-buffered regions

The PS4 app composites into a cached CPU-RAM shadow framebuffer and caches rasterized glyphs,
background ground, scaled covers and dimmed wheel pill backdrops. Display-buffer damage is tracked
independently for both VideoOut buffers across flips so that neither back buffer retains stale pixels
from two frames ago; only damaged rectangles are streamed write-only to the target display buffer
with zero reads from VideoOut memory. Input handling, holds and wheel pointing never wait on HTTP
requests or artwork decoding.
Check: test (pixel-exact double-buffering equivalence, damage/cache reduction metrics and zero
blocking calls on input/render in `test/panel_render_test.c`; no NetCtl or IME call blocks the frame
loop in `test/panel_lane_test.c`). Real frame pacing requires a PS4 smoke test.
Source: PS4 rc.3 performance report (slow L2 wheel and UI caused by full-frame redraws, per-frame
FreeType rasterization and alpha-blending reads on write-combining direct memory) (2026-11-27).

### PANEL-13: web panels warn when PS4Mount coexistence holds automatic registration

When `GET /api/games` reports `coexist.auto_register_blocked: true`, both the modern panel (`web/index.html`)
and the legacy panel (`web/legacy.html`) show a prominent Spanish/English warning banner explaining that
PS4Mount is still installed or active in GoldHEN AutoRun, that automatic registration is paused so the
shared library is not re-registered twice, and that the user should remove PS4Mount or disable it in
AutoRun before continuing (Lodestar never deletes PS4Mount's files). When `coexist.auto_register_blocked`
is false or absent, the banner stays hidden.
Check: test (`test/web/panel.test.js` in both modern and legacy panels, English and Spanish).
Source: environment coexistence hazard review (`hazards/environment`), 2026-11-27.
