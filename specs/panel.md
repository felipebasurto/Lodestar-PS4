# Library panel

### PANEL-1: the library is a dense artwork grid, not a featured-game shelf

The web panel and the PS4 app present the library as a grid of compact game artwork across the main content area.
No persistent inspector, hero panel or selected-game preview may take library space away from the
rest of the collection. Keyboard/controller focus and the current filter survive library refreshes.
Opening a game's details is an explicit action and makes no write request. Adding, removing and
restoring games continue to use their existing actions inside the details surface.
Check: test for focus, details requests and filtering (`test/web/panel.test.js`), read for layout and focus geometry.
Source: user direction (2026-11-26): a Steam-like library; a large visualization of one game hides the rest.

### PANEL-2: the console app can navigate and go back

The app supports directional focus and confirm on available controls. Back closes the current
dialog or page before it exits the panel. The legacy HTML panel keeps its own Back handling.
Check: test for the keyboard route and fallback (`test/web/panel.test.js`), read for gamepad input.
Source: user request for TV and controller operation.

### PANEL-3: game artwork comes first without sacrificing scanability

The grid shows artwork, titles, and states that need attention in compact cards. Details use the
large artwork and expose package paths and maintenance actions there. The modern panel shows the
search field, the state/platform filters and the drives' free space above the grid, the frequent
actions (scan, send games, download links, notifications) in the header, and the rest (settings,
remove several, log, stop) in one menu. Back closes an open menu before leaving the PS4 panel, and in
the PS4 app Up/Down move out of the search field so it never traps the controller.
Check: test for reaching details, filtering and Back (`test/web/panel.test.js`), read for hierarchy.
Source: user feedback on the blue shelf and the Steam-like library direction; user (2026-11-26):
a clean, well organised interface (search and the main actions were hidden behind two menus).

### PANEL-4: write requests come only from local network addresses

The panel has no password. A write request must carry the panel header and come from loopback,
a private LAN range, link-local, carrier-grade NAT space, or a local socket. Public IPv4 addresses
are refused before any file or settings write.
Check: test (keep bar 2: security boundary). Test: `test/unit.c` › `test_web_net`.
Source: existing panel threat model.

### PANEL-5: the PS4 tile opens a native controller interface

The standalone PS4 app uses VideoOut and Pad. It renders the library, game details and
commonly used settings over a dark blue background with the Lodestar star, a library
sidebar and a persistent controller legend. It can also queue one download link at a time
and show download progress and recent jobs. It reads data and submits actions exclusively
through the same loopback HTTP API used by the web panel. Network requests and artwork
decoding run off the input/render loop. Library refreshes preserve selection by title ID.
Adding, removing and restoring use the existing API and require explicit confirmation
for removal and restore. Circle backs out of a subpage before quitting the app locally.
The mobile/desktop HTML panel remains the place for local file uploads, bulk download
management and advanced administration. At phone widths, search spans the content,
the header controls can wrap, and the connection indicator stays visible. The legacy HTML URL and close route remain compatible with older clients.
Check: test for the native client's response parsing and HTTP boundary; read for render,
focus, process ownership and route use. VideoOut, Pad and the system keyboard require a
real PS4 smoke test before release.
Source: user approval of the 1080p Lodestar mockup and native PS4 requirement (2026-11-26).
