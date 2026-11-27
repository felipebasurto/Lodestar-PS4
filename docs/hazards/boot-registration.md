# Boot and registration hazards

Nothing in this document was run on a console. The line numbers are from this branch.
Host tests read the sources; they do not call Sony APIs.

## Resumen

El 27 de noviembre de 2026, v3.1.0-rc.3 en una PS4 con firmware 14.00 y GoldHEN hizo que
ShellCore marcara `app.db` como corrupta. La consola reconstruyó la base y los juegos del
disco interno desaparecieron del menú. En el primer arranque Lodestar abrió `LODS00001` con
`user_id` 0, porque `sceUserServiceGetForegroundUser` se llamó sin mirar el retorno y la
variable ya valía 0. ShellCore buscó `tbl_appbrowse_0000000000` y levantó `DbCorruptionOccured`.
A los 120 s, sin nadie en el panel, se guardaron los ajustes por defecto y se registraron unos
20 juegos encima.

Esa clase de fallo es: un retorno de una API del sistema ignorado, un parámetro que se queda
en 0 o en «vacío», y a continuación una escritura del estado de la consola (lanzar, registrar,
desregistrar, montar) sin un consentimiento explícito.

Los cinco más graves, en este árbol:

1. **Lanzar e instalar con usuario 0.** Pérdida de catálogo. Probabilidad alta en el primer
   arranque, antes de que haya un usuario en primer plano. Ya lo cubren el PR #15 (rechaza
   cualquier id `<= 0xFF`) y el PR #16 (rechaza 0, negativos, `0xFE` y `0xFF`, pero **acepta
   1..0xFD**). Este PR no lo vuelve a arreglar. Prueba `expected-fail` H1.
2. **A los 120 s se guardan los ajustes por defecto y empieza el registro masivo.** Mutación
   del catálogo sin consentimiento. Alta. Lo cubren #15 y #16. Este PR no quita el temporizador.
   Solo impide que un `config.txt` a medias cuente como consentimiento (P4N-32). Prueba H2.
3. **El registro sigue aunque `app.db` esté marcada o se haya leído vacía.** Un fallo de lectura
   ya espera (`scan_all`). Una lectura correcta de un catálogo vacío, o un evento
   `DbCorruptionOccured`, no. Alta. Solo lo cubre #16 (se detiene). #15 avisa del fichero
   `app.db-corrupt` y sigue registrando. Prueba H3.
4. **Montar el escenario de `/user/app` cuando la tabla de procesos no se pudo leer, o cuando
   solo está abierto `LODS00001`.** Las dos lecturas fallidas devuelven 0, igual que «no hay
   ninguna app». Alta o media: esconde la biblioteca mientras corre `AppInstallAll`, y el panel
   se queda abierto en cuanto el PR #18 deje de morir por `SIGSYS`. **Ningún PR cubre las dos
   mitades.** #15 hace que un `sysctl` fallido devuelva -1 y sigue sin ver el panel. #16 ve
   cualquier `eboot.bin`, panel incluido, y sigue tratando el fallo de `sysctl` como 0. Pruebas
   H4a y H4b.
5. **A los 120 s se desmonta `/user/app` aunque `AppInstallAll` siga en marcha.** El mismo
   temporizador que ya se vio colgar tras el reposo. Alta o media: la biblioteca sigue oculta o
   ShellCore recorre un árbol que acaba de cambiar. **Ni #15 ni #16 lo quitan.** Prueba H5.

Lo nuevo respecto de #15–#18 es el punto 4 (el hueco entre los dos PRs), el punto 5, la carrera
del desinstalador (H6, H6b), `db_is_registered` usado como prueba de ausencia (H7, en contra de
P4N-2, que no se ha tocado), un `getfsstat` fallido que parece «el juego no está en marcha»
(H8), y el arreglo pequeño de P4N-32. #17 no corrige producción: deja escritas las decisiones
inseguras para poder testarlas. #18 evita el `SIGSYS` al salir de la app; no rechaza el usuario 0.

No existe la rama `integrate/first-run-safety-panel-rescue`.

## What this hunt looked for

The incident class is an ignored Sony return, a zero or empty out-parameter, and then a change
to console state that nobody asked for. The scope is the boot path, the foreground user, the
`LODS00001` launch, timeouts and defaults that register or unmount, `sceAppInstUtil*` /
`AppInstallAll` / BGFT, retries, and error paths whose return code is dropped.

Open PRs on `main`, read so this branch would not redo them:

| PR | Branch | What it already changes |
| --- | --- | --- |
| #15 | `cursor/first-run-registration-safety-2eb3` | Foreground user must be `> 0xFF`. No 120 s auto-save. `open_apps` returns -1 when `sysctl` fails. Version re-add uses `db_confirms_registered`. `app.db-corrupt` is logged and does not stop registration. |
| #16 | `fix/db-corruption-guardrails` | Rejects user 0, negatives, `0xFE` and `0xFF`, and accepts 1..0xFD. A corruption latch stops registration, re-add, uninstall, BGFT and the panel launch. The 120 s timeout cannot save. Staging treats any `eboot.bin` as busy. `open_apps` still returns 0 on `sysctl` failure. |
| #17 | `test/hardwareless-gauntlet` | Host harness. Its boot decisions keep today's unsafe launch and the 120 s save on purpose. Not a fix. |
| #18 | `fix/panel-app-rescue` | The panel stops returning from `main` (that was `SIGSYS`). `GetInitialUser` still accepts user id 0. |

Upstream `Vld439/PS4Mount-PS4` commit `a5eadb64` already has the same shape: user id initialized
to 0 and `sceUserServiceGetForegroundUser` ignored at `src/main.c:1551`, `:5267` and `:5380`;
`sceLncUtilLaunchApp` only printed at `:5383`; both `Initialize` calls ignored at `:6228`;
`SETUP_WAIT` then `config_save()` at `:6318`. Lodestar added the automatic `LODS00001` launch
on top of that inherited path. The launch is what makes user 0 reach ShellCore's
`tbl_appbrowse_%010u` lookup. A real local user id is above `0xFF` (commonly `>= 0x10000000`).
`0`, negatives, `0xFE` (`EVERYONE`) and `0xFF` (`SYSTEM`) are not profiles.

## Sony API returns

Direct calls. "Discarded" means the return is not stored and not tested. Printing it is not a
check: that is how the 27 Nov launch logged its own failure and continued.

| Call | Where | On failure / 0 / unexpected |
| --- | --- | --- |
| `sceUserServiceGetForegroundUser` | `src/main.c:2224`, `:6878`, `:7025` | Return discarded. `user` is set to 0 first (`:2223`, `:6877`, `:7024`). 0 is passed to `install_pkg` or `sceLncUtilLaunchApp`. |
| `sceLncUtilLaunchApp` | `src/main.c:7027` | Only printed. `opened = 1` at `:7022` already consumed the only attempt. |
| `sceUserServiceInitialize` | `src/main.c:7482` | Return discarded. Argument is `0`, not an init-params struct. |
| `sceAppInstUtilInitialize` | `src/main.c:7483` | Return discarded. |
| `sceAppInstUtilAppInstallAll` / `AppInstallTitleDir` | via `g_install_all` / `g_title_dir`, `src/main.c:3164`–`:3172` | Return stored in `g_install_rc`. A busy call is not started twice. `REG_NONE` returns -1. The pointer call is invisible to the name-based policy check. |
| `sceAppInstUtilAppUnInstall` | via `g_uninstall_call`, `src/main.c:5684` | Return stored, then the thread is detached (`:5706`). After 60 s the caller returns -2 (`:5709`) while the thread can still finish. |
| `sceKernelGetAppInfo` | `src/main.c:6049` | Checked (`!rc && app_id`). |
| `sceSystemServiceKillApp` | `:6074`, `:6089` | Printed. The restore path then waits. User-initiated. |
| `sceKernelLoadStartModule` (`libSceRegMgr`) | `:219` | Checked (`lh > 0`). |
| `sceRegMgrGetInt` / `sceSystemServiceParamGetInt` | `:224`, `:226` | Checked. |
| `sceKernelSendNotificationRequest` | `:337` | Return discarded. `user_id` left 0, `target_id` -1. This is the usual broadcast notification. It does not look up `tbl_appbrowse_*`. Not ranked. |
| `sceSystemServicePowerTick` | `:7344` | Return discarded. No catalog change. |
| `sceKernelLoadStartModule` (`libSceBgft`) | `src/install.c:49` | Return discarded. The next handle lookup fails closed (`:51`). |
| BGFT init / register / start | `src/install.c:73` onward, called through pointers | Returns stored and branched. `install_pkg` copies the caller-supplied user id (`:98`) and does not check it. |
| `sceNetPoolCreate` / `ResolverCreate` / `StartNtoa` | `src/https.c:54`–`:61` | Checked. `ResolverDestroy` ends the resolver. |
| `sceUserServiceInitialize` | `panel-app/main.c:834` | Return discarded. |
| `sceSystemServiceParamGetInt` | `panel-app/main.c:835` | Return discarded. Language falls back to the initializer. |
| `sceSysmoduleLoadModule` | `:837`, `:838` | Return discarded. A missing module fails later, inside font or IME init. |
| `sceUserServiceGetInitialUser` | `:839` | Checked only for `rc < 0`. User id 0 continues into `scePadOpen` (`:842`) and the IME dialog. |
| `scePadInit` / `scePadOpen` / `scePadReadState` | `:840`, `:842`, `:729` | Checked. |
| `sceImeDialogInit` / `GetResult` / `GetStatus` | `:399`, `:406`, `:746` | Checked. `sceImeDialogTerm` (`:433`, `:795`) discarded. |
| `sceNetCtlInit` / `GetInfo` | `:584` | Checked. |
| `sceVideoOutOpen` / flip / buffers / memory | `:799`–`:823` | Checked, except `sceSystemServiceHideSplashScreen` (`:815`) and `sceVideoOutClose` on the way out. |
| `main` returns | `panel-app/main.c` | Process exit from the title is `SIGSYS`. Owned by #18. |

`test/hazard_api.py --policy` freezes the direct `sceUserService*` / `sceLncUtil*` /
`sceAppInstUtil*` findings in `test/hazard_api_baseline.txt`. A new discarded call fails the
policy. Removing one without deleting its baseline line fails it too. Calls through a function
pointer are not in that list; the two registration pointers above do store their return.

## What changes console state

Automatic, no POST and no button:

| Action | Where | Notes |
| --- | --- | --- |
| Launch `LODS00001` | `src/main.c:7019` | First start, once, when `db_is_registered` says the tile exists and `open_games` returns 0. |
| Download the panel pkg | `src/main.c:7031` | 15 s after boot if the tile is missing. The install then uses the user id from `install_received`. |
| Save defaults and scan | `src/main.c:7557` | 120 s with nobody at the panel. The scan registers preexisting games in groups of `batch_size` (default 5). |
| Mask and unmask `/user/app` | `src/main.c:3333`, `:3396` | `nullfs` of the stage dir over `/user/app` for `AppInstallAll`, then a force unmount. |
| `AppInstallAll` | `src/main.c:3172` | Writes system install state. The only other writer of that state is BGFT. |
| BGFT task | `register_finish` → `install_title_dlc`; `dlc_install`; `install_received` | Add-ons and a pkg sent as "install". External mode does not copy add-ons. |
| Rewrite `param.sfo` | `src/main.c:3114` | `/system_data/priv/appmeta/<TID>/param.sfo`. `write_file`'s return is ignored. #15 stops the "larger than 8 KiB looks different" rewrite loop. |
| Retry a failed register | `src/main.c:3932` | Every 300 s. |
| Re-add on version mismatch | `src/main.c:4296` | Uses `db_app_ver`, not `db_confirms_registered`. #15 changes that call. |
| Delete-window remount | `src/main.c:7537` | After 600 s the games are mounted again if the close line was missed. Low probability. If the user is still on the delete screen, a pkg can be exposed to the system delete. Not changed here; the delete spec owns that window. |
| Drop external add-on mounts | `release_dlc_mounts`, called from `scan_all` | When the saved mode is no longer external. |
| `autorun_apply(1)` at boot | `src/main.c:7493` | Only when the saved settings already say so. The default is off. |

Explicit (POST or the panel's own control): add, remove, restore, save settings, rescan, stop,
upload-and-install, close the panel app.

## Hazards

Severity is what a console can lose. Likelihood is on a first boot or a later scan of this tree,
before #15–#18 are merged. "Covered" means an open PR removes the bug pattern the host test
looks for. A covered row is still an `expected-fail` test here, so merging the other PR without
dropping the mark fails CI.

| Id | Hazard | Severity | Likelihood | Evidence | Covered by |
| --- | --- | --- | --- | --- | --- |
| H1 | Launch and BGFT with user id 0 | Catalog loss (`DbCorruptionOccured`, home menu rebuilt) | High on first boot before a foreground user | `src/main.c:2223`, `:6877`, `:7024`, `:7482`; `src/install.c:98` | #15 (id `> 0xFF`). #16 rejects 0 / `0xFE` / `0xFF` and still accepts 1..0xFD. The test goes green when the `user = 0` call disappears, so the 1..0xFD gap has to be read in #16's `dbguard_user_ok`, not in this test. |
| H1b | Panel `GetInitialUser` accepts 0 | Defense in depth after a bad launch. Pad and IME run as user 0 | Lower once H1 is fixed | `panel-app/main.c:831`, `:839`, `:842` | Not #18. |
| H2 | 120 s timeout saves defaults and mass-registers | Catalog mutation without consent. This is the ~20 games after the wipe | High on a first start nobody attends | `src/main.c:193`, `:7557` | #15 and #16. This branch still calls `config_save` there. |
| H3 | Registration continues on a flagged or emptied `app.db` | Catalog loss, a second wipe | High if ShellCore is already rebuilding | `scan_all` waits only when `!db_ready()` (`src/main.c:4719`). `install_all` counts `!db_is_registered` (`:3371`). No corruption latch anywhere in `src/` | #16 halts. #15 logs the marker only. |
| H4a | Failed process-table read looks like "no app" | Staging and the auto-launch both proceed | Medium. Becomes the common case if `sysctl` fails open | `open_apps` returns 0 at `src/main.c:6036` and `:6041`. The launch uses that via `open_games` at `:7023` | #15 returns -1. #16 still returns 0. |
| H4b | A running `LODS00001` does not block staging | `/user/app` is hidden under the panel for up to two minutes | Medium, and higher once #18 stops the panel crashing on start | `mask_user_app` calls `open_games` (`src/main.c:3336`), which excludes the panel | #16 passes panel `< 0`. #15 still calls `open_games`. |
| H5 | Force-unmount `/user/app` while `AppInstallAll` is still in the 120 s wait | Library stays hidden; ShellCore walks a tree that just changed. Same family as the rest-mode hang that put this call on a thread | Medium on a slow register | `src/main.c:3394`–`:3397` | Neither #15 nor #16. Follow-up. |
| H6 | `readd_title` always calls `app_add` | A failed uninstall is followed by a new registration of the same id | Medium, on "system file disagree" | `src/main.c:5815`, `:5825` | Neither. #16 skips the whole readd only while its latch is set. |
| H6b | Uninstall thread outlives the 60 s wait | `app_remove` puts `.src` back, then `AppUnInstall` still completes | Medium, together with H6 | `src/main.c:5706`, `:5709`; rollback at `:5780` | Follow-up. Not in #15–#18. |
| H7 | `db_is_registered` used as proof of absence | A failed read looks unregistered: uninstall is skipped and `clean_holds` runs (`rc == 0`) | Medium on a failed `app.db` read outside `scan_all` | `src/main.c:5774`. Also the need-count at `:3371`, `register_finish` at `:3412`, the mask filter, and the launch gate at `:7019`. `db_confirms_*` exist (`src/db.c:168`) and are not used here. `src/db.h:11` already says nothing may be decided until a read has succeeded | Follow-up. P4N-2 already states the rule. This PR does not edit P4N-2 to match the call sites. |
| H8 | `getfsstat` failure clears the sandbox list | `title_running` returns 0. A later unmount can treat a running game as stopped | Low, and only if the kernel log also missed the launch | `src/main.c:2523`–`:2526`, `:2548` | Follow-up. Not in #15–#18. |
| H9 | `Initialize` returns discarded | Services used before they are up. The user-0 launch does not need this to happen, and it makes it easier | Medium on a process that already initialized UserService (the documented code is `0x80960003`) | `src/main.c:7482`, `:7483`; `panel-app/main.c:834` | #15 logs the payload UserService init only. |
| H10 | Launch return only printed | The tile does not open and nothing says why, on this tree | Medium-low once H1 and H2 are fixed (the user still has the web panel) | `src/main.c:7027` | #15 stores the return and warns. #16 still prints it. |
| H10b | `opened = 1` before `LaunchApp` returns | A failed launch is not tried again | Same as H10 | `src/main.c:7022` then `:7027` | Follow-up. #15 still sets the flag first. |
| H11 | No login-user list and no user-switch handling | Add-ons are queued for whoever is foreground at that instant. A guest id is a real id (`> 0xFF`), so it passes #15. Wrong account, not a catalog wipe | Low | no `sceUserServiceGetLoginUserIdList` in `src/` or `panel-app/` | Follow-up. |

The settings-file bug that this branch does fix is not in that ranking. It is the next section.

## The one code change (P4N-32)

`write_file` opens with `O_TRUNC` and returns -1 on a short write, leaving whatever it already
wrote (`src/main.c:429`). `config_save` used to call `setup_set(0)` and then ignore
`write_file`. `first_start` treats any existing `config.txt` as "not a first start"
(`src/main.c:6394`). A partial file would have ended the first run on the next boot, and a
failed write would already have opened registration.

`config_save` now writes `config.txt.tmp`, renames it onto `config.txt`, and only then calls
`setup_set(0)` (`src/main.c:6422`, `:6431`). Failure unlinks the temporary file, leaves the
previous `config.txt` in place, and returns -1. `app_config_set` (`:6651`) says the settings
were not saved, puts the in-memory settings back, and puts GoldHEN's AutoRun list back when
this save had changed it. The 120 s path (`:7558`) logs that no game is added yet. It still
calls `config_save`. Removing that timeout belongs to #15 and #16.

The consent check in `test/hazard_api.py` failed on the old function (consent cleared before
the write, no error return, both callers discarded it) and passes on this one. The old shape
is still rejected by `test/hazard_boot_test.py`, so the check cannot go quiet just because the
live function moved.

P4N-2 was re-read. It already says a failed or incomplete database read is unknown, never proof
of absence. The call sites in H7 disagree with that rule. The spec was not edited to allow them.

## Follow-ups, not done here

These are the fixes this hunt thinks should land, in the order of the table. They are left out
because #15 or #16 already owns them, or because they are not a one-function change.

1. One staging gate that does both halves of H4: `open_apps` returns -1 when the process table
   cannot be read (#15), and the mask treats every `eboot.bin`, including `LODS00001`, as busy
   (#16). Shipping either PR alone leaves one half open. The launch at `src/main.c:7023` needs
   the same "unreadable is not empty" result.
2. After the 120 s wait, if `g_install_busy` is still set, leave `/user/app` masked and say so
   (H5). Do not force-unmount under a live `AppInstallAll`.
3. On uninstall timeout, do not `pthread_detach` a thread that can still call `AppUnInstall`.
   Do not restore `.src` while that call is in flight. `readd_title` must not call `app_add`
   when `app_remove` failed (H6, H6b).
4. Decisions that uninstall, register, mask or launch must use `db_confirms_registered` /
   `db_confirms_unregistered` (H7). A read that has not succeeded does none of those.
5. `refresh_sandboxes` must keep the previous list when `getfsstat` fails (H8).
6. The panel must refuse a user id `<= 0xFF` before `scePadOpen` (H1b). #18 does not.
7. Whichever user check ships, use #15's bound (`> 0xFF`), not #16's (1..0xFD accepted).
8. Set `opened = 1` only after `sceLncUtilLaunchApp` returns success (H10b).
9. Check `sceAppInstUtilInitialize` and the panel's `sceUserServiceInitialize` (H9).
10. There is still no read of who is logged in, and no reaction to a user switch (H11).

## Tests

`bash test/policy.sh` runs `python3 -I test/hazard_api.py --policy`.

`bash test/run.sh` runs `python3 -I test/hazard_boot_test.py`. Required checks pass. Each
`expected-fail:` line is a bug that is still in this tree; the script exits 0 only while those
assertions fail. An unexpected pass (the bug pattern disappeared) exits non-zero until the mark
is removed.

No console, no package, no firmware image and no key was used. Comparing with #15–#18 was a
`git show` / `git grep` of those branches. The upstream lines are from
`Vld439/PS4Mount-PS4` `a5eadb64` `src/main.c`.
