# System-State Consistency and Interruption Hazards

Review of PR #22 (`hazards/system-state`) verified against `integrate/first-run-safety-panel-rescue`
(PR #19) and `integrate/hazards-review`.

## Verdict table

| ID | Hazard claimed in PR #22 | Verdict | Evidence & action on `integrate/hazards-review` |
| --- | --- | --- | --- |
| SYS-1 | `db.c` publishes torn/shrunk or first `nolock=1` reads while `app.db-journal` / `app.db-wal` exists | **Real — fixed here** (`SYS-1`) | Added `src/dbsnap.h` (`db_snapshot_publish`) and journal/WAL + empty-catalog checks in `src/db.c`. Keeps prior snapshot with `g_read_valid = 0` on rejection. Tested in `test/db_test.c` and `test/sysstate_test.c`. |
| SYS-2 | `write_file()` opens live destination with `O_TRUNC` and ignores `EINTR` | **Real — fixed here** (`SYS-2`) | Added `src/durable.{h,c}` (`durable_replace` via `mkstemp` + `write` loop + `fchmod` + `fsync` + `rename`) and wired into `write_file()` and `build.sh`. Tested in `test/sysstate_test.c`. |
| H1 | Unbound `AppUnInstallMainExecute ret = [0]` commits pending user delete of `<root>/ps4/<TID>` even while `app.db` still shows the title registered | **Real — fixed here** (`DEL-6`) | Updated `specs/delete.md` `DEL-6` and `src/delete.h` (`delete_pending_decision`): destructive local deletion now requires fresh `db_state == 2 && seen_registered`; unbound `report == 0` never commits on its own. Tested in `test/unit.c`. |
| H2 | `enforce_patch_sfo` replaces `/system_data/priv/appmeta/<TID>/param.sfo` while a nullfs hides `/user/app/<TID>/app.pkg` | **Real — open (hardware-dependent)** | Detecting an underlying internal `/user/app/<TID>/app.pkg` beneath an active nullfs mount cannot be done safely without unmounting or kernel-specific lookup; kept documented as open. |
| H3 | `place_files` writes appmeta/license/playgo before `AppInstallAll` and does not roll back on failure | **Real — open (risky without console evidence)** | Auto-deleting system appmeta or shared license files on failed `AppInstallAll` risks removing state belonging to an existing or partially registered title. Kept documented as open. |
| H4a | `unmask_user_app` clears `g_masked` even if `/user/app` remains masked | **Already fixed by #19** (`REG-9`) | `dbguard_unmask_user_app()` (`src/dbguard.c`) clears `*masked_flag` and `STAGE_DIR` only when `is_masked` confirms `/user/app` is unmasked. |
| H4b | `shutdown_daemon` does not unmask `/user/app`; `keep_awake` ignores active `AppInstallAll` / pending DLC | **Real — fixed here** | `shutdown_daemon` calls `unmask_leftover()` before closing sockets when `user_app_masked() && !g_install_busy`; `keep_awake` also stays awake while `g_install_busy` or `g_ndlc_pending > 0`. |
| H4c | `.removed` written before `uninstall_title` | **Real — intentional trade-off (`DEL-5`)** | Writing `.removed` before `uninstall_title` prevents a concurrent scan from re-mounting the title mid-uninstall, and `app_remove` rolls it back on failure. Kept documented as open trade-off. |
| H5 | Two-file commits (`*.rif`/`*.idx`, `d0.pdb`/`d1.pdb`) are non-atomic across crashes | **Real — open (OS/filesystem boundary)** | POSIX/PS4 UFS has no atomic multi-file rename across two separate filenames; ShellCore/BGFT require both fixed filenames. Kept documented as open. |
| Lower | `addcont_installed` treats any non-dot directory entry in `/user/addcont/<TID>` as installed | **Real — open (hardware-dependent)** | Checking exact `ac.pkg` layout across firmware/add-on variants requires console evidence; kept documented as open. |
| 19 XFAILs in #22's `sysstate_test.c` | Substring `strstr` checks on `src/main.c` (8 already fixed by #19, 2 fixed here, rest open hardware items) | **Cleaned up** | Replaced brittle source-substring scraping with behavioral tests for `SYS-1`, `SYS-2`, `DEL-6`, and `REG-9`. Genuinely open hardware-dependent items remain documented above rather than asserted via fragile `strstr` probes. |
