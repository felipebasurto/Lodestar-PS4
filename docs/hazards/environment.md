# Environment and PS4Mount Coexistence Hazards

Review of PR #20 (`hazards/environment`) verified against `integrate/first-run-safety-panel-rescue`
(PR #19) and `integrate/hazards-review`.

## Verdict table

| ID | Hazard claimed in PR #20 | Verdict | Evidence & action on `integrate/hazards-review` |
| --- | --- | --- | --- |
| ENV-1 | Lodestar re-registers `<drive>/ps4/<TID>` titles owned by PS4Mount because its BGFT URL and `/data/lodestar/<TID>.src` differ | **Real — fixed here** (`P4N-32`, `REG-12`, `PANEL-13`) | `src/coexist.{h,c}` probes PS4Mount payloads (`ps4mount-ps4.bin/.elf` >= 4 KiB with ELF/bin magic), `/user/app/PSMT00001`, `/user/app/PMNT00001`, and active `payloads.ini` entries read-only. `scan_root` (`src/main.c`) holds automatic registration and version-mismatch re-add while active; explicit single-title `POST /api/mount` remains allowed via `coexist_title_register_allowed()`. Added `src/coexist.c` to `build.sh` (missing in PR #20). |
| ENV-2 | Auto-launch of `LODS00001` with `user_id = 0` | **Already fixed by #19** (`P4N-28`, `REG-6`) | `foreground_user_id()` (`src/main.c`) uses `boot_launch_user()` requiring `rc == 0 && user > 0xFF`. |
| ENV-3 / ENV-TIMEOUT / ENV-CLOCK | 120 s first-start timeout auto-saves defaults or trips early on wall-clock step | **Already fixed by #19** (`P4N-29`, `REG-8`) | The 120 s timeout was removed completely from `main()` in #19. Stale `coexist_setup_timeout_due` code from #20 was dropped. |
| ENV-4 | `payloads.ini` keeps PS4Mount while inserting Lodestar first; lock-conflict blame | **Partially fixed by #19, closed here** (`P4N-19`, `P4N-31`, `P4N-32`) | #19 fixed lock-conflict attribution (`P4N-31`). Fixed stale comment in `src/autorun.h`. Coexistence guard (`P4N-32`) blocks auto-register whenever PS4Mount is in AutoRun. |
| ENV-5 / ENV-DOUBLE-LOAD | Duplicate payload load hijacks process-wide `stdout` via `log_start()` / `log_stop()` before acquiring lock 9032 | **Real — fixed here** | `main()` now acquires `single_instance()` before calling `log_start()`. On lock conflict, `report_lock_conflict()` writes one stamped line via `log_append_line()` (`src/logts.c`) without touching `fd 1`. |
| ENV-MIGRATE | `migrate_organized()` moves `<drive>/ps4/<TID>` when `organize=0` even while PS4Mount is present | **Real — fixed here** (`P4N-32`) | `migrate_organized()` now checks `coexist_migrate_allowed(g_cfg.organize, dbguard_halt_active(), auto_register_blocked())`. |
| ENV-RESTART-CLOCK | Wall-clock jump > 60 s (e.g. NTP sync after boot) causes `needs_reboot()` to unlink `<TID>.restart` | **Real — open (hardware-dependent)** | `boot_time()` (`src/main.c`) derives wall boot time from `time(NULL) - CLOCK_UPTIME` because `kern.boottime` is unavailable to payloads; in-run `g_reboot_tids` still holds it in RAM, so only cross-reload within the same boot after a > 60 s wall jump is affected. Left open pending console-safe boot-id mechanism. |
| ENV-LEFTOVER | XFAIL in #20 asserting leftover `/data/ps4mount` alone should block auto-register | **Rejected (spec contradiction)** | Directly contradicts `P4N-32` and `P4N-27`: Lodestar never deletes `/data/ps4mount`, so blocking on leftover state alone would permanently wedge users who uninstalled PS4Mount. Recorded as `ps4mount_state` without blocking. |
| Mock stubs in #20 | `coexist_same_process_reentry_safe`, `coexist_second_load_stops_log`, `coexist_manual_register_allowed`, etc. | **Rejected (slop / unused stubs)** | Functions in #20's `src/coexist.c` were never called by `src/main.c` and returned hardcoded constants solely for `env_test.c` XFAILs. Dropped; real production decision helpers are tested instead. |

## Rule ID reconciliation

- `P4N-32`: assigned to the PS4Mount coexistence guard (PR #21's atomic `config_save` moved to `P4N-33`).
- `REG-12`: renumbered from PR #20's `REG-11`, which collided with existing `REG-11` from PR #19.
- `PANEL-13`: ES/EN coexistence banner in `web/index.html` and `web/legacy.html`.
