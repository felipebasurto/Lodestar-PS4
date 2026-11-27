# System-state hazards

Host analysis of Lodestar on `main` after the 27 Nov 2026 firmware 14.00 + GoldHEN run of v3.1.0-rc.3 (app.db marked corrupt, rebuilt, internal-storage games gone from the home menu). This document is the next set of ways a mount, register, delete, or scan can leave the console inconsistent. It is not a console test. ShellCore's rebuild behavior is labeled **inference** where it is not something this tree measures (P4N-12).

Rules closed here: `specs/system-state.md` SYS-1 and SYS-2. Everything else in the tables is either already covered by open PRs #15–#18 or is a follow-up. Lodestar stays read-only on system databases (`test/policy.sh`).

## Resumen

En la PS4 del 27 nov 2026, ShellCore marcó `app.db` como corrupta después de abrir la app del panel con `user_id` 0, y un arranque en espera registró solo los juegos de USB. Eso ya lo cubren los PR #15 y #16. Este informe busca el siguiente corte a medias.

Los cinco huecos que siguen abiertos, de mayor a menor:

1. **Borrar la carpeta del juego por un resultado de desinstalación que no nombra ese juego.** Si hay un borrado del usuario pendiente y el kernel escribe `AppUnInstallMainExecute ret = [0]` por cualquier otro título, Lodestar borra `<unidad>/ps4/<TID>` (la única copia si los pkg se organizaron ahí). El pkg suelto, fuera de esa carpeta, se queda.
2. **Reescribir el `param.sfo` privado de una instalación real** mientras un nullfs tapa su `app.pkg`. Tras reiniciar, el juego interno sigue en disco con el sfo del dump.
3. **Dejar metadatos a medias** (appmeta, licencia, playgo, carpeta vacía en `/user/app`) si el registro se corta antes de que el sistema cree la fila. Un rebuild no los convierte en juego y puede leerlos mal. No hay marcha atrás.
4. **Parar, reposo, cierre o recarga de GoldHEN en medio.** Parar no desmonta `/user/app`. El reposo no se aplaza durante una instalación. `.removed` se escribe antes de que el sistema termine de quitar el juego.
5. **La pareja de archivos no es un solo commit.** Un corte entre `rif` e `idx`, o entre `d0.pdb` y `d1.pdb`, deja las dos mitades de generaciones distintas. Un solo archivo ya no puede quedar a la mitad (SYS-2).

Cerrado en este PR, sin probarlo en consola: SYS-1 (una lectura más corta de `app.db` mientras existe `app.db-journal` no demuestra que un juego desapareció) y SYS-2 (el reemplazo de un archivo no lo trunca). La lista de inventario de más abajo es solo lectura.

## Ranked hazards

Likelihood is host-inferred from how wide the window is, not from a count of consoles.

| # | Hazard | Severity | Likelihood | Evidence | Where the fix belongs |
| --- | --- | --- | --- | --- | --- |
| 1 | An uninstall result of 0 commits deletion of the pending title's library folder without checking that the line names that title | Library loss for an organized `<root>/ps4/<TID>` (that folder is the pkg). In-place pkgs stay; a `.removed` mark is written instead | Medium: any other `AppUnInstallMainExecute ret = [0]` while a user delete is pending (re-add, DLC, another registration) | `src/delete.h:96`, `src/main.c:5254-5258`, `src/main.c:5113-5121` | Follow-up. DEL-6 still treats a zero result as commit. Not in #15–#18 |
| 2 | `enforce_patch_sfo` replaces `/system_data/priv/appmeta/<TID>/param.sfo` while a nullfs hides `/user/app/<TID>/app.pkg`, so a real install of the same title is invisible | Unplayable internal game after reboot: the pkg is still there, the sfo is the dump's | Low–medium: only when that title is also a real install, or becomes one while the mount is up | `src/main.c:2986-2992`, `src/main.c:3102`, `src/main.c:3404`, `src/main.c:4277-4281` | Follow-up. #15 only fixes the read of an sfo larger than 8192 bytes |
| 3 | Registration writes system metadata and mounts before `AppInstallAll`, and failure returns without removing them | Catalog: orphan appmeta, license, playgo, hold, empty `/user/app/<TID>`. A rebuild does not turn those into a home-menu row and can misread a short or foreign sfo | Medium: every failed or interrupted register | `src/main.c:2295-2316`, `src/main.c:2381-2391`, `src/main.c:2797-2810`, `src/main.c:4979-5019` | Follow-up. #16's inventory records Lodestar's own rows; it does not remove these |
| 4 | Stop, rest, crash, or GoldHEN reload mid-operation | Catalog hidden while `/user/app` stays masked (until reboot or the next Lodestar start). Rest can cut a long install. `.removed` before uninstall sticks a tile that will not be mounted again | Medium for a stop or reload during the install window; low for the `.removed` crash window | `src/main.c:7157-7170`, `src/main.c:7298-7309`, `src/main.c:5760-5762`, `src/main.c:7463`, `src/main.c:3298-3301` | Split. Unmask-while-still-mounted is #15 and #16. Shutdown, rest, and marker order are follow-ups |
| 5 | License `rif`/`idx` and BGFT `d0.pdb`/`d1.pdb` are two commits. BGFT reads tasks only at boot | Unplayable: a shared 19-character content-id prefix can take a torn license; a torn BGFT pair sits until reboot and then still will not load | Low–medium: the gap is two writes | `src/main.c:1649`, `src/main.c:2133-2140`, `src/build.c:362-363` (an invalid old pair is dropped, so the next successful write keeps only the new slot) | Follow-up for the pair. SYS-2 closes the single-file short write |

Lower, still real:

| Hazard | Severity | Evidence | Fix |
| --- | --- | --- | --- |
| A non-empty `/user/addcont/<TID>` with any non-dot name counts as a finished internal add-on, so a partial directory blocks the external mount | Add-on never mounts | `src/main.c:2887-2912`, `src/main.c:2920` | Follow-up |
| `mkdir` of `/user/app/<TID>` (and patch/addcont parents) persists after the nullfs is gone | Empty directory left for recovery to see | `src/main.c:2810` | Follow-up; the checklist only records it |
| USB unplug leaves a current hold mounted (dead symlink). Legacy `/mnt` sources are released when `app.pkg` is gone | Launch fails until the drive returns or the console reboots. Not file loss | `src/main.c:4453-4477` (`strncmp(from, "/mnt/", 5)`) | Accepted (MNT-5) |
| Full disk during a replace | Before SYS-2, `O_TRUNC` could leave a short file. A failed `durable_replace` leaves the previous file | `src/durable.c`, `src/main.c:431` | SYS-2 |
| Same-count torn read (one title dropped, another added) while the journal exists | Absence of the dropped title can still be trusted | `src/dbsnap.h` | Residual, stated in SYS-1 |
| A shrink with the journal already gone (a finished rebuild) is trusted, so the next scan can register every external title | Catalog flood, the incident's second half, once the corrupt marker has been cleared | `src/main.c:4707`, `src/dbsnap.h` | #16 pilot. #15 stops the first-start timeout only |

Saves under `/user/home` and `/user/data` are not written by any step in this model. Whether a database rebuild moves or drops them is unknown here. The checklist tells you to look, and to leave them.

## What this PR changes

**SYS-1.** `db_snapshot_publish` (`src/dbsnap.h`, used from `src/db.c:130`). The first successful read of a run is not published while `app.db-journal` exists. A later successful read with fewer titles than the last published snapshot is not published while that journal exists. The previous list stays. `db_confirms_registered` and `db_confirms_unregistered` both fail for that read, so a pending delete cannot commit on the shrink. A shrink after the journal is gone is published, including an empty catalog. Growth and an equal count are published even if the journal exists. Lodestar still does not write the database or delete the journal.

**SYS-2.** `write_file` (`src/main.c:431`) is `durable_replace`. The new bytes go to `<path>.tmp.<random>` in the same directory, are `fsync`'d, and `rename` over the destination. The destination is not opened with `O_TRUNC`. Failure before rename leaves the previous directory entry and removes the temp. A crash before rename can leave the temp sibling; the path itself is the previous file. An existing regular file keeps its mode. A symlink is replaced, not followed. This does not make a pair of files one commit (hazard 5).

No change to the delete decision, the mask flag, the user-id check, `SETUP_WAIT`, or when `enforce_patch_sfo` writes.

## Coverage by open PRs

| Incident / gap | #15 first-run safety | #16 db guardrails | #17 host gauntlet | #18 panel rescue | This PR |
| --- | --- | --- | --- | --- | --- |
| `user_id` 0 launching `LODS00001` | Fixes | Fixes (`foreground_user_id`) | Documents, no production change | No | Expected-fail on current `main` (`src/main.c:6988-6991`) |
| `user_id` 0 for DLC install and for a pkg sent from the panel | Fixes | Fixes | Documents | No | Expected-fail (`src/main.c:2211`, `src/main.c:6841`) |
| First start waits 120s, then saves defaults and scans | Removes that path | Removes it | Documents | No | Expected-fail (`src/main.c:7521`). After `config.ini` exists, #15 does not cap the next boot |
| Scan while `app.db-corrupt` is set | Diagnostics | Halt latch | Documents | No | Expected-fail (`src/main.c:4707` only checks `db_ready`) |
| `unmask` clears `g_masked` and calls `stage_clear` while `/user/app` is still masked | Fixes | Fixes (`dbguard_unmask_user_app`) | Documents | No | Expected-fail (`src/main.c:3298-3301`) |
| `open_apps` returns 0 when `sysctl` fails, so the mask can hide `/user/app` while a game is open | Fixes | Fixes | Documents | No | Expected-fail (`src/main.c:6023`) |
| `install_all` unmasks after 120s and continues while `AppInstallAll` is still busy | No | Aborts the batch and restores `/user/app` first | Documents | No | Expected-fail (`src/main.c:3382-3387`) |
| After a rebuild, with the marker gone, the next scan registers every external title | No (not a first start) | One pilot title, then an explicit continue | Documents | No | Expected-fail (no `pilot_tid` / `BREAKER_PILOT` on this tree) |
| Unbound uninstall commit (hazard 1) | No | No | No | No | Expected-fail, follow-up |
| priv `param.sfo` overwritten under a nullfs (hazard 2) | Larger sfo read only | No | No | No | Expected-fail, follow-up |
| Metadata left behind (hazard 3) | No | Inventory file, not a rollback | No | No | Expected-fail, follow-up |
| Stop / rest / `.removed` order (hazard 4, the parts #15/#16 do not take) | No | No | No | No | Expected-fail, follow-up |
| rif/idx and d0/d1 pairs (hazard 5) | No | No | No | No | Expected-fail, follow-up. Single-file truncate is SYS-2 |
| Torn shrink of `app.db` while the journal exists | No | No | No | No | **Fixed, SYS-1** (host test only) |
| `O_TRUNC` of a file Lodestar replaces | No | No | No | No | **Fixed, SYS-2** (host test only) |
| Panel SIGSYS / exit rendering | No | No | No | Fixes | Untouched |

#17 already compared this tree with upstream `Vld439/PS4Mount-PS4` at `a5eadb6`. Inherited from that line: user id 0, the first-start timeout, clearing the mask flag after a failed unmount, and `AppInstallAll` over a masked `/user/app`. This hunt does not repeat that diff. `av_content_bg.db` does not appear in `src/`. `addcont.db` is only `stat`'d for mtime (`src/main.c:3218`) so Lodestar can wait while the system writes; its rows are never read or written here.

Branch `integrate/first-run-safety-panel-rescue` was not on the remote when this was written.

## State machines

A step is the state after that action and before the next. **Death** is crash, kill, or GoldHEN reload: process memory is gone, nullfs remains. **Stop** calls `shutdown_daemon` (`src/main.c:7476`) which closes sockets and does not unmount; the mask remains the same way. **Reboot** drops nullfs and keeps files. **Rebuild** is a reboot plus the inference that external `AppInstallAll` rows are gone from `app.db` while pkgs, holds, appmeta, licenses, and playgo files stay. The next Lodestar start calls `unmask_leftover` only when `statfs` shows `/user/app` still mounted from the stage (`src/main.c:7463`), so a leftover mask is visible to the next process even if the previous one cleared `g_masked`.

### Register

| Step | Left behind | Death | Reboot | Rebuild | Idempotent to retry? |
| --- | --- | --- | --- | --- | --- |
| idle | nothing of ours | same | same | same | yes |
| meta-temp | `<file>.tmp.<random>` only; the path is the previous file or absent (SYS-2) | same | same | same | yes: retry replaces or leaves the previous file |
| meta | `/user/appmeta/<TID>`, `/system_data/priv/appmeta/<TID>/*`, `/system_data/playgo/<TID>`; no `app.db` row | orphans, no tile | same | recovery can see the sfo and still have no tile | retry overwrites; it does not delete the orphans if it fails again (H3) |
| lic-rif | `free<19>.rif` is the new pair's rif; `.idx` is still the previous one | a shared prefix can break another install's license | same | same | no, until idx is committed (H5) |
| lic | both new | previous slots kept only if `build_license` could read the old pair | same | same | a later success rewrites the pair |
| hold | symlink under `/system_data/lodestar` | hold remains | same | same | yes |
| mounted | nullfs of the hold on `/user/app/<TID>`, plus the directory `mkdir` made | that title's real directory is hidden | mount gone; empty directory remains; no row | same | remount of an exact top is already-done (`mount_prepare_step`) |
| masked | `/user/app` shows only the batch | other games hidden until reboot or the next start | mask gone | mask gone | stop does not enter this cleanup (H4) |
| installing | masked, `AppInstallAll` in flight, row not assumed | system may still be registering after the process dies | a row may exist with no mount: broken tile until the next scan | row dropped (inference); pkg remains | #16 aborts instead of continuing |
| row + mask | row exists, still masked | tile may show; other games hidden | broken tile until the next scan | tile gone (inference) | retry of an existing row is the system's `0x80990002` on TitleDir |
| unmasked | row, mount, no `.src` yet | playable while the mount lives | broken tile until the next scan | tile gone; files remain | the next scan remounts if the pkg is still there |
| marker | `.src` written | playable while the mount lives | next scan remounts from `.src` | tile gone; `.src` remains, so the next scan can register again, uncapped on this tree | repeating the finished op does not delete the library or the save |

`place_files` failure at the license (`src/main.c:2316`) returns before the `out:` label (`src/main.c:2381`), which only frees buffers.

### Delete (console delete screen)

The pending flag is memory. Death before a commit keeps the folder. `delete_game_files` (`src/main.c:5091`) deletes only a path that is byte-exactly `<root>/ps4/<TID>`.

| Step | Library folder | In-place pkg | Death / reboot | Rebuild |
| --- | --- | --- | --- | --- |
| intent (`GameWillDelete` only) | kept | kept | flag forgotten; the game can be added again. Files stay | same |
| unmounted, still registered | kept | kept | launch fails until a later mount | row may also be gone (inference) |
| some uninstall returned 0, title not checked | **deleted today** | kept, `.removed` written | icon can remain with the folder gone | same, and the row may still be there | 
| fresh read: was registered, now absent | deleted (DEL-6) | `.removed` | intended when the absence is real. SYS-1 blocks this while the journal is still there | a finished rebuild (journal gone) is trusted |
| uninstall returned 0 and the line names this title | deleted | `.removed` | intended | intended |
| 600s with no commit | kept | kept | rollback only | same |

Hazard 1 is the third row. The fifth row is not reachable today, because the handler never reads a title id out of the line (`src/main.c:5254`).

### Panel remove (`app_remove`)

This path does not delete the library folder. It asks the system to unregister, then `clean_holds`.

| Step | Markers | Menu | Next scan |
| --- | --- | --- | --- |
| detached, markers still intact | `.src` present | row remains, mount down | remounts |
| `.removed` written, `.src` unlinked, uninstall not finished | `.removed` | row remains | will not mount it again until an explicit add (H4) |
| uninstall returned -2 (timeout) and markers were restored | `.src` restored | the system thread can still finish and drop the row | can register the title again |
| uninstall returned 0 | holds cleaned; `.badpatch` kept | row gone if the system agreed | pkgs were not deleted by this function |

`clean_holds` removes playgo only when the title is not internal and `db_confirms_unregistered` (`src/main.c:5012`). It does not remove `/user/appmeta`, `/system_data/priv/appmeta`, or `/user/license`.

### Scan

| Gate | What this tree does | After reboot | After rebuild |
| --- | --- | --- | --- |
| `db_ready()` is 0 | returns without registering (`src/main.c:4707`) | waits again | waits until a read is trusted |
| journal present and the list shrank, or this is the first read | previous list kept; absence not confirmed (SYS-1) | the next process has no previous list, so a first read with a leftover journal is also withheld | a journal left forever in one run defers catalog changes until the journal is gone or the payload is loaded again |
| corrupt marker set, read still succeeds | scan proceeds | same | #16 stops this; this tree does not |
| read succeeds, journal gone, fewer titles | the new list is the catalog, including empty | external titles look unregistered | the next scan can register all of them (#16 caps that at one pilot) |

### Mount

| Situation | Result | Interrupted | Reboot |
| --- | --- | --- | --- |
| live top is already the exact hold | `MOUNT_PREPARE_ALREADY`; doing it again is the same | no write | mount is gone; the next scan mounts again |
| no layers | mount | `mkdir` of the target already happened | empty directory remains |
| foreign or unknown layer | left in place (MNT) | no unmount | kernel drops it on reboot, not us |
| USB gone, source is `/system_data/lodestar/...` | left mounted | dead symlink; launch fails | mount gone |
| USB gone, source is `/mnt/...` and `app.pkg` is gone | `release_stale` unmounts | that legacy mount is dropped | already gone if the reboot happened first |
| `/user/app` masked | `detach_target_mode` and `mount_dir` return success without changing that title (`src/main.c:2758`, `src/main.c:2798`) | intentional while the flag is true. If the flag was cleared while the mount remains (#15/#16), later mounts no longer skip | mask gone |

## Idempotency that already holds

- Exact-top mount and a second call (`mount_prepare_step` → `MOUNT_PREPARE_ALREADY`).
- Unmount when the table shows no layers (`MOUNT_STEP_DONE` again).
- A staged pkg copy is deleted only when the title is confirmed registered, the target is unmounted, the target is a regular file, and the sizes match (`stage_installed_ok`). Missing any one of those keeps the copy.
- Console delete of a path that is not `<root>/ps4/<TID>` is refused (`delete_target_ok`).
- A pending delete that ages out rolls back. Time is not a reason to delete (`src/delete.h:100`).
- Death during `GameWillDelete` and before a commit keeps the library folder.
- Register and delete, at every modeled step, leave saves in place and do not write `app.db` from Lodestar.
- A durable replace of an existing file, killed before rename, leaves the previous bytes (host test). Replacing a symlink does not write through it.
- Two finished registers do not delete the library.

## What a rebuild drops, and what Lodestar leaves

**Inference, not measured on a console.** A ShellCore rebuild replaces `app.db` / `appinfo.db` rows. It does not delete USB pkgs, hold directories, or the metadata files listed below. FPKG rows that existed only because `AppInstallAll` saw a nullfs are the ones the 27 Nov incident lost from the home menu; the pkgs were still on disk.

| Survives reboot | Does not survive reboot | A rebuild can misread |
| --- | --- | --- |
| Regular files under `/user/app`, `/user/patch`, `/user/addcont` | nullfs mounts, `g_masked`, `g_will_delete` | A priv `param.sfo` Lodestar replaced (hazard 2, or a crash before SYS-2) |
| `/user/appmeta`, `/system_data/priv/appmeta` | in-memory BGFT id set (the on-disk task remains; BGFT reads it at the next boot) | A `rif` without a matching `idx` (hazard 5) |
| `/user/license`, `/system_data/playgo`, `/user/playgo` | the stage mount | An empty `/user/app/<TID>` that was only a `mkdir` |
| `/user/bgft/task/<id>/{d0,d1}.pdb` until the next boot consumes install tasks | | A playgo directory for a title that has no row |
| `/system_data/lodestar` holds and stage directories (files, not mounts) | | A `.src` whose title is no longer in `app.db`: the next scan treats it as needing register |
| `/data/lodestar` (`.src`, `.removed`, `.restart`, `.badpatch`, `config.ini`, `log.txt`) | | |
| The pkg on the USB drive or in `<root>/ps4/<TID>` unless hazard 1 committed | | |

`av_content_bg.db` is not read or written by Lodestar. If the system rebuilt it, that was not this payload's write. `addcont.db` rows for DLC the system installed via BGFT are the system's; Lodestar's external DLC is a mount, and the mount is gone after reboot.

## Read-only inventory after a rebuilt catalog

Do this before loading Lodestar again. On this tree the next scan can register every external title once `app.db` reads (the cap is #16's pilot, not `main`). Do not open the system databases in a tool that takes a lock: a lock on `app.db` is how ShellCore has been made to flag it corrupt. Do not unlink, remount, re-register, or edit anything in this list. Names, sizes, types, and times are enough. Saves are looked at, not judged.

1. `/system_data/priv/mms/` — names, sizes, mtimes of `app.db`, `appinfo.db`, `addcont.db`, `av_content_bg.db`, each `*-journal`, `*-wal`, and `*-corrupt`. A `*-corrupt` file means the system already flagged that database. Do not open them.
2. `/user/app`, `/user/patch`, `/user/addcont` — for each title directory, record whether `app.pkg` / `patch.pkg` / `ac.pkg` is a regular file, a symlink, a directory, or missing, and whether the directory is empty. If `mount` still shows nullfs on these paths, GoldHEN was reloaded or the payload stopped without a reboot; the mount will not be there after a reboot.
3. `/user/appmeta/<TID>` and `/system_data/priv/appmeta/<TID>` — a directory with no regular `app.pkg` under `/user/app/<TID>` is metadata Lodestar or the system left behind. Note `param.sfo` size. Do not delete it from this list.
4. `/user/license/` — pairs `free<prefix>.rif` and `free<prefix>.idx`. A rif without its idx, or a sibling whose name contains `.tmp.`, is an interrupted replace. The file without `.tmp.` is the one the system reads. Leave both.
5. `/system_data/playgo` and `/user/playgo` — title directories whose title is not in the home menu.
6. `/user/bgft/task/` — each id should have `d0.pdb` and `d1.pdb`. One without the other is a torn task. BGFT will not reread it until the next boot.
7. `/system_data/lodestar/` — `app/`, `patch/`, `addcont/`, `stage/`. After a reboot, `stage/` is files, not a live mask. A non-empty `stage/` means an install was in progress.
8. `/data/lodestar/` — `log.txt`, `log.txt.old`, `config.ini`, `*.src`, `*.removed`, `*.restart`, `*.badpatch`. In the log, `[db]`, `[reg]`, `[delete]`, `[clean]`, `[panel-app]`, and `corrupt` say what the last run did. `*.removed` means a later scan will not mount that title until it is added again. `*.src` means Lodestar still considers the title its own.
9. The USB drive and each `<root>/ps4/<TID>/` — the pkg is here when the home menu no longer shows the game. Missing here and missing as a regular file under `/user/app` is the case to treat as the pkg actually gone (hazard 1 if a delete was in progress). Do not remove the folder from this list.
10. `/user/home` and `/user/data` — list the save directories for the titles you care about. This hunt does not know whether the rebuild touched them. Do not move them.

When the list is done, a game missing from the menu with its pkg still at step 9 is the rebuild case: the row is gone, the file is not. Loading Lodestar will try to register external titles again. That is a separate decision from this inventory.

## Lista de inventario (solo lectura)

Hazlo antes de volver a cargar Lodestar. En este árbol, la siguiente búsqueda puede registrar todos los juegos externos en cuanto `app.db` se lea (el tope de un solo título piloto está en el PR #16, no en `main`). No abras las bases del sistema con una herramienta que las bloquee. No borres, no montes, no registres y no edites nada de esta lista. Bastan nombre, tamaño, tipo y hora. Las partidas se miran, no se dan por perdidas ni por guardadas.

1. `/system_data/priv/mms/` — nombre, tamaño y hora de `app.db`, `appinfo.db`, `addcont.db`, `av_content_bg.db`, y de cada `*-journal`, `*-wal` y `*-corrupt`. Un archivo `*-corrupt` significa que el sistema ya marcó esa base. No los abras.
2. `/user/app`, `/user/patch`, `/user/addcont` — en cada título, anota si `app.pkg` / `patch.pkg` / `ac.pkg` es un archivo normal, un enlace, un directorio o no está, y si la carpeta está vacía. Si `mount` aún muestra nullfs, GoldHEN se recargó o el payload se detuvo sin reiniciar. Tras un reinicio ese montaje no sigue.
3. `/user/appmeta/<TID>` y `/system_data/priv/appmeta/<TID>` — una carpeta sin un `app.pkg` normal en `/user/app/<TID>` es metadatos que se quedaron. Anota el tamaño de `param.sfo`. No la borres siguiendo esta lista.
4. `/user/license/` — parejas `free<prefijo>.rif` y `free<prefijo>.idx`. Un rif sin su idx, o un hermano cuyo nombre contiene `.tmp.`, es un reemplazo cortado. El archivo sin `.tmp.` es el que lee el sistema. Deja los dos.
5. `/system_data/playgo` y `/user/playgo` — carpetas de títulos que ya no están en el menú.
6. `/user/bgft/task/` — cada id debería tener `d0.pdb` y `d1.pdb`. Uno solo es una tarea a medias. BGFT no la vuelve a leer hasta el próximo arranque.
7. `/system_data/lodestar/` — `app/`, `patch/`, `addcont/`, `stage/`. Tras reiniciar, `stage/` son archivos, no una máscara activa. Si `stage/` tiene algo, había un registro a medias.
8. `/data/lodestar/` — `log.txt`, `log.txt.old`, `config.ini`, `*.src`, `*.removed`, `*.restart`, `*.badpatch`. En el registro, `[db]`, `[reg]`, `[delete]`, `[clean]`, `[panel-app]` y `corrupt` dicen qué hizo la última carga. `*.removed` significa que la siguiente búsqueda no montará ese título hasta que se añada a mano. `*.src` significa que Lodestar sigue considerándolo suyo.
9. El USB y cada `<raíz>/ps4/<TID>/` — el pkg está aquí cuando el menú ya no muestra el juego. Si tampoco está como archivo normal en `/user/app`, entonces el pkg sí falta (el riesgo 1, si había un borrado a medias). No borres la carpeta siguiendo esta lista.
10. `/user/home` y `/user/data` — lista las partidas de los títulos que te importan. Este análisis no sabe si el rebuild las tocó. No las muevas.

Si el juego no está en el menú y el pkg sigue en el paso 9, es el caso del rebuild: falta la fila, no el archivo. Cargar Lodestar intentará registrar otra vez los juegos externos. Eso es otra decisión, distinta de este inventario.

## Tests

`test/sysstate_test.c` walks every register and delete step for stay, death, reboot, and rebuild, and checks that saves, the organized library (except a commit), and the pkg stay, that Lodestar does not write `app.db`, and that a durable replace is never a short file. `test/db_test.c` holds a writer transaction so `app.db-journal` exists, checks that absence is not confirmed, then commits a real shrink with the journal gone and checks that absence is confirmed.

`EXPECTED-FAIL` lines are desired properties this tree does not meet. The process still exits 0. Nineteen of them are asserted. If one starts holding, that test fails so it can be promoted to a normal check. The nineteen name hazard 1 (behavior and the kernel-log handler), hazard 2, hazard 3, hazard 4 (shutdown, rest, `.removed` order), hazard 5 (license pair and BGFT pair), the unmask flag, the 120s install continue, three `user_id` 0 sites, `open_apps` fail-open, the first-start auto-save, `scan_all` ignoring a corrupt marker, the missing post-rebuild cap, and a partial addcont directory.

Nothing in this document was run on a PS4.
