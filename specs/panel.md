# Library panel

### PANEL-1: the library is a dense artwork grid, not a featured-game shelf

The web panel and the PS4 app present the library as a full-width grid of compact game artwork.
No persistent inspector, hero panel or selected-game preview may take library space away from the
rest of the collection. Keyboard/controller focus and the current filter survive library refreshes.
Opening a game's details is an explicit action and makes no write request. Adding, removing and
restoring games continue to use their existing actions inside the details surface.
Check: test for focus, details requests and filtering (`test/web/panel.test.js`), read for layout and focus geometry.
Source: user direction (2026-11-26): a Steam-like library; a large visualization of one game hides the rest.

### PANEL-2: the console app can navigate and go back

The app supports directional focus and confirm on available controls. Back closes the current
dialog or page before it exits the panel, including when the app uses the ES5 fallback.
Check: test for the keyboard route and fallback (`test/web/panel.test.js`), read for gamepad input.
Source: user request for TV and controller operation.

### PANEL-3: game artwork comes first without sacrificing scanability

The grid shows artwork, titles, and states that need attention in compact cards. Details use the
large artwork and expose package paths and maintenance actions there. The modern panel keeps
library filters and management in separate menus. Back closes an open menu before leaving the PS4 panel.
Check: test for reaching details, filtering and Back (`test/web/panel.test.js`), read for hierarchy.
Source: user feedback on the blue shelf and the Steam-like library direction.

### PANEL-4: write requests come only from local network addresses

The panel has no password. A write request must carry the panel header and come from loopback,
a private LAN range, link-local, carrier-grade NAT space, or a local socket. Public IPv4 addresses
are refused before any file or settings write.
Check: test (keep bar 2: security boundary). Test: `test/unit.c` › `test_web_net`.
Source: existing panel threat model.
