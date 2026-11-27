# Environment hazards

Lodestar began as a port of gasb07/PS4Mount and is GPL-3.0-or-later. This note is about the
machine it runs on: another copy of that port, GoldHEN, rest mode, the firmware, an old
settings file, and the clock. It does not describe a console run. Nothing here was loaded
on a PS4.

The code cited is this branch. `main` at `61d48a4` (v3.1.0-rc.3) has the same paths except
the guard marked **this PR**. Open pull requests #15, #16, #17 and #18 are unmerged; where
they already own a fix, this branch does not repeat it.

## Resumen

El 27 de noviembre de 2026 la consola (firmware 14.00, GoldHEN) tenía Lodestar 3.1.0-rc.3 y
también el port PS4Mount (app `PSMT00001`, atajo `PMNT00001`, puertos 8770/8771). Lodestar
usa `LODS00001` y los puertos 8780/8781. Los dos se excluyen con un candado en
`127.0.0.1:9032`. Lodestar no borra `ps4mount-ps4.bin` del AutoRun de GoldHEN.

La consola marcó `app.db` como dañada, la reconstruyó y los juegos internos desaparecieron
del menú. La causa principal que se sospecha es abrir la app con `user_id` 0 y, además, el
registro automático a los 120 segundos. Eso lo cubren los PR #15 y #16, que no están
fusionados.

Lo que nadie había mirado: la carpeta `<disco>/ps4/<TID>` es de los dos. Si PS4Mount ya
registró esos juegos, Lodestar igual los ve como nuevos, porque la tarea BGFT y el fichero
`.src` son los suyos, no los de PS4Mount, y los vuelve a registrar. Este PR corta ese
registro automático cuando el payload, la app, el atajo o una línea activa del AutoRun de
PS4Mount están presentes, y lo dice en el panel en español y en inglés. No se ha probado
en la consola.

Sigue abierto, y los tests lo marcan como fallo esperado: el tiempo de 120 segundos cuando
PS4Mount no está (lo quitan #15 y #16), un salto del reloj que tira la marca de reinicio,
una segunda carga en el mismo proceso que cierra el log de la primera, mover `ps4/` si
«organizar» está apagado, y el directorio viejo `/data/ps4mount` solo, sin app ni AutoRun.
Añadir un juego a mano desde el panel sigue permitido.

## The five that matter most

1. **Re-registering the shared `<drive>/ps4/<TID>` library** while PS4Mount is installed or
   listed in AutoRun. Critical, and likely on a console that still has that port.
   `title_state` (`src/main.c:3058`) queues a title that is already in `app.db` when the
   BGFT task does not name Lodestar's pkg URL, or the `.src` under `/data/lodestar` does
   not match. PS4Mount's task and marker live elsewhere, so the title looks new.
   `titles_on_root_dirs` (`src/main.c:3993`) reads `<root>/ps4` even when the scan folder
   is `Lodestar`. The scan then calls `AppInstallAll`. **This PR** stops that scan, the
   version-mismatch re-add, and the first-start save that would lead to a scan, when the
   probe sees the payload, the app, the shortcut, or an active AutoRun line (P4N-32).
   #15–#18 do not look for PS4Mount. An explicit Add in the panel still registers one
   title. Leftover `/data/ps4mount` alone does not trip the guard.

2. **Launching the panel app with `user_id` 0.** Critical, and the suspected primary cause
   of the `app.db` rebuild. `panel_app_step` sets `user` to 0 and still calls
   `sceLncUtilLaunchApp` with whatever `sceUserServiceGetForegroundUser` left there
   (`src/main.c:7066`). #15 (P4N-28) and #16 (REG-6) own this. This PR does not.

3. **The 120-second first-start wait, and a clock step that fires it at once.** High, and
   likely when nobody saves settings. `SETUP_WAIT` is 120 (`src/main.c:194`). The loop
   saves defaults when wall time passes that wait (`src/main.c:7607`), and the next scan
   registers. `boot_time` is wall minus monotonic (`src/main.c:1921`), so a forward step
   of the clock also expires the wait. #15 (P4N-29) and #16 remove the timeout. This PR
   skips the save only while the PS4Mount guard is on. The cases without PS4Mount are
   expected-fail, owned by those PRs.

4. **AutoRun lists both payloads, and Lodestar is inserted first.** High once the user
   turns Lodestar's AutoRun on. `autorun_is_ours` matches only `lodestar-ps4.bin` and
   `.elf` (`src/autorun.c:15`). `ps4mount-ps4.bin` stays (P4N-19). `autorun_edit_ini`
   inserts Lodestar directly under `[AutoRun]` (`src/autorun.c:65`), so the next boot can
   give Lodestar the 9032 lock and then, before this guard, the scan. This PR blocks the
   registration. The lock failure still says "PS4Mount or another mount daemon"
   (`src/main.c:7525`); #15 (P4N-31) is the message fix. #17's audit calls this partial
   and does not change the payload.

5. **A second load in the same process calls `log_stop`.** Medium, possible if GoldHEN
   injects the payload again while the first `main` is still in its loop. `log_start`
   runs before the lock (`src/main.c:7518`, `src/logts.c:107`) and `dup2`s stdout.
   Failure then calls `log_stop` (`src/main.c:7527`, `src/logts.c:141`), which in the
   same process restores the winner's stdout and joins the wrong thread. A second
   process is safe: the bind fails and only the loser exits. #15 still calls `log_stop`;
   it changes the text. Expected-fail, owner open.

Next, not in those five: a wall step over 60 seconds drops `.restart` marks
(`src/main.c:1950`, expected-fail, open); `migrate_organized` still runs when organize is
off (`src/main.c:7637`, expected-fail, open); the panel's Add stays available; klog port
3232 is exclusive and on by default (`src/main.c:4861`).

## Matrix

Expected means the safe behavior. Actual is what this branch does. Likelihood assumes a
console that still has PS4Mount, GoldHEN AutoRun, or an old Lodestar config, which is the
machine from 27 Nov 2026.

| Condition | Operation | Expected | Actual | Severity | Likelihood | Covered by |
| --- | --- | --- | --- | --- | --- | --- |
| PS4Mount payload, app `PSMT00001`, or shortcut `PMNT00001` present | Scan registers `<drive>/ps4/<TID>` | Do not auto-register; say so in the panel, EN and ES | Guard on: pending titles are not sent to `AppInstallAll` (`src/main.c:4314`). Banner: `web/index.html`, `web/legacy.html` | Critical | Likely | **This PR** (P4N-32, REG-11, PANEL-13). Not #15–#18 |
| Same, title already in `app.db` but BGFT URL or `.src` is Lodestar's | `title_state` queues it | Do not register | Same guard. Without the guard, `src/main.c:3069` returns 1 for "no game task naming its pkg" or "its pkgs changed" | Critical | Likely | **This PR**. #15/#16 stop the timeout and user 0, not this mismatch |
| Active AutoRun line `ps4mount-ps4.bin` or `.elf` | Same scan, and first-start save | Do not auto-register | Guard on (`src/coexist.c` line match; `src/main.c:7608` skips `config_save`) | Critical | Likely | **This PR** |
| Lodestar AutoRun turned on while PS4Mount's line remains | Who binds 9032 | One daemon. Registration still off if the line remains | Lodestar is inserted first (`src/autorun.c:65`) and can win the lock. Registration is off while the line remains | High | Likely | Registration: **this PR**. Message when the lock is lost: #15 |
| PS4Mount already holds 9032 | Second load, other process | Do not start | `bind` without `SO_REUSEADDR` fails (`src/main.c:7250`, `LOCK_PORT` `src/main.c:81`). Loser calls `log_stop` and returns | Low (the interlock works) | Likely | P4N-27. Message: #15 |
| Same-process re-injection | `log_start` / `log_stop` | Return before touching the running log | `src/main.c:7518` then `src/main.c:7527` | High | Possible | Open (ENV-DOUBLE-LOAD). #15 does not remove `log_stop` |
| Only `/data/ps4mount` or `/system_data/ps4mount` | Auto-register | Block as well, if that state means the other product was here | Recorded as `ps4mount_state`, does not set `block` (`src/coexist.c`) | High | Possible | Open (ENV-LEFTOVER), by choice of P4N-32 |
| Explicit Add in the panel | One title | User's action; still able to re-register a shared title | Allowed (`coexist_manual_register_allowed`) | Medium | Possible | Documented residual. Not a scan |
| First start, PS4Mount absent, 120 s with no save | Save defaults and scan | Never adopt on a timeout | Still saves (`src/main.c:7616`) | High | Likely | #15 P4N-29, #16. Expected-fail ENV-TIMEOUT |
| Wall clock jumps forward | That same wait | Do not treat a step as consent | `coexist_setup_timeout_due` is wall time. Guard holds only if PS4Mount is already detected | High | Possible at boot | #15/#16 (removing the wait). Expected-fail ENV-CLOCK |
| Wall clock jumps more than 60 s | `.restart` mark | Keep the mark; the game still needs a restart | `needs_reboot` unlinks it (`src/main.c:1955`) | Medium | Possible | Open (ENV-RESTART-CLOCK). Not #15–#18 |
| Rest mode, lock socket dies, other daemon took 9032 | Resume | Stop, do not share the mount table | `src/main.c:7564` rebinds or stops | Low (correct) | Possible | P4N-27 |
| Rest mode, `AppInstallAll` blocks | The daemon loop | Keep the panel alive | The call runs on a helper thread (`src/main.c:3158`) | Low (already split) | Seen once, comment in the source | Already in `main`. #16 fails the batch closed on staging errors |
| Organize off, PS4Mount present | `migrate_organized` | Do not move `<drive>/ps4` | Still called (`src/main.c:7637`) | Medium | Unlikely (default organize is on; ver &lt; 2 forces it on, `src/main.c:6544`) | Open (ENV-MIGRATE) |
| Upgrade from ver &lt; 2 or ver &lt; 3, or a PS4Mount `config.ini` | `config_load` / `first_start` | Do not treat the other product's files as ours, and do not mass-register | PS4Mount's config is not read (P4N-27). `first_start` ignores `/data/ps4mount` (`src/main.c:6459`). ver &lt; 2 sets organize on. ver &lt; 3 maps `dlc=1` to external | Medium | Likely on an upgrade | Specified behavior. The mass-register that follows is row 3, unless the guard is on |
| Firmware 13.52 and 14.00 | Which register call | A 14.00-specific path only with evidence | `fw_reg_mode` returns `REG_ALL` for major ≥ 6 (`src/fw.c:19`). Symbols are `dlsym` at runtime (`src/main.c:3146`). `app.pbm` is written as 13.52 left it (`src/main.c:2345`) | Unknown | Both firmwares use one path | No proven bug. 14.00 task bytes are not verified here |
| Ports 8780, 8781, 9032, 3232 beside 8770, 8771 | Bind | Panel ports do not collide; the lock does, on purpose | 8780 `src/web.h`, 8781 `src/links.h`, 9032 `src/main.c:81`, klog 3232 `src/main.c:4870`. 8780 has no fallback (`src/web.c:43`): if it is taken, the daemon still runs and can still register | Medium for 8780 busy; none for 8770 | 9032 collision is the design. 8780 collision is uncommon | Lock: P4N-27. 8780 busy is open, not this guard |
| klog on (the default) and another payload needs 3232 | Connect | Leave the port when the user asks | Default on. Failure falls through to `/dev/klog` (`src/main.c:4878`). The panel already says to turn it off for a payload that needs it | Low | Possible with several payloads | User setting. Not #15–#18 |
| #18 panel-app changes | This daemon | No overlap | #18 does not change `src/main.c` | — | — | #18 is the native app's exit and drawing |

## What this branch changes

P4N-27 used to say Lodestar may scan and serve `<drive>/ps4/<TID>`. It now says automatic
registration is forbidden while PS4Mount is installed or in AutoRun. P4N-32, REG-11 and
PANEL-13 are the new rules. WEBAPI-7 gains an additive `coexist` object on `GET /api/games`
and `GET /api/status`. The ids sit past the ones #15, #16 and #18 already use (P4N-31,
REG-10, PANEL-12), so a later merge can keep one sequence.

The probe is `stat` and a read of `payloads.ini` only. It does not write `app.db`, does not
delete PS4Mount's files, and does not remove its AutoRun line. A payload counts when it is a
regular file of at least 4 KiB and starts with ELF or UH, the same check Lodestar uses for
its own file. A commented AutoRun line does not count. The shortcut paths tried are
`/user/app/PMNT00001`, `/system_data/priv/appmeta/PMNT00001` and `/user/appmeta/PMNT00001`;
if a console keeps that shortcut elsewhere, the payload file and the AutoRun line still
trip the guard.

## Expected-fail

`test/env_test.c` prints `EXPECTED-FAIL` and still exits 0. If the safe condition becomes
true, the test fails until the mark is removed.

| Id | Owner | What is still wrong |
| --- | --- | --- |
| ENV-TIMEOUT | #15 #16 | Without PS4Mount, the 120 s wait still saves defaults |
| ENV-CLOCK | #15 #16 | A forward wall step expires that wait immediately |
| ENV-RESTART-CLOCK | open | A wall step over 60 s drops the `.restart` mark |
| ENV-DOUBLE-LOAD | open | A second `main` in the same process still calls `log_start` and `log_stop` |
| ENV-MIGRATE | open | Organize off still moves `<drive>/ps4` while PS4Mount is present |
| ENV-LEFTOVER | open | `/data/ps4mount` alone does not block auto-register |

The 9032 bind, the 13-vs-14 registration mode, the upgrade mapping, and "manual add stays
allowed" are checks of current behavior, not expected failures.
