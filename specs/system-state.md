# System-state durability and snapshot safety spec

Governs: `src/dbsnap.h`, `src/db.c`, `src/durable.h`, `src/durable.c`, `src/main.c` (`write_file`),
`test/db_test.c`, `test/sysstate_test.c`.
Sources: system-state interruption and consistency review (`hazards/system-state`, `hazards/boot-registration`),
2026-11-27.

## Purpose

Lodestar reads ShellCore's SQLite databases without locks (`mode=ro&nolock=1`, P4N-30) and writes its
own state and hold metadata under `/data/lodestar` and `/system_data/lodestar`. A torn `app.db` read
during a ShellCore transaction or rebuild, or a half-written state file interrupted by a power cut or
crash, must fail closed instead of triggering mass re-registration or losing configuration.

## Rules

### SYS-1: never publish an empty `app.db` read or a first/shrunk read while a journal is active
`db.c` opens `/system_data/priv/mms/app.db` strictly read-only with `mode=ro&nolock=1` so it never
blocks ShellCore's writes. Because `nolock=1` can observe a mid-transaction or mid-rebuild file,
`db_snapshot_publish` (`src/dbsnap.h`) rejects a candidate snapshot when:
- the SQLite query did not finish with `SQLITE_DONE`,
- the query returned zero `TITLE_ID` rows (a live PS4 `app.db` always contains system titles; an
  empty-but-successful read after a rebuild or torn read must never make `db_ready()` true or confirm
  any title absent), or
- `app.db-journal` or `app.db-wal` is present on disk and either no prior snapshot exists yet or the
  new title count is smaller than the prior snapshot's count.

When a candidate snapshot is rejected, `db.c` keeps the previous committed snapshot in memory and
leaves `g_read_valid = 0` so neither `db_confirms_registered` nor `db_confirms_unregistered` treats
the rejected or stale state as fresh proof. Lodestar never writes, locks, or deletes any database or
journal file under `/system_data/priv/mms/`.
Check: test (keep bar 1: SQLite boundary; keep bar 2: data/system DB safety; keep bar 3: concurrency).
Test: `test/db_test.c`, `test/sysstate_test.c`; compile boundary `test/sdkcheck.sh`.
Source: `hazards/system-state` SYS-1 and `hazards/boot-registration` H3 (empty `app.db` read), 2026-11-27.

### SYS-2: state and metadata writes replace files atomically via temp + `fsync` + `rename`
`write_file()` (`src/main.c`) delegates to `durable_replace()` (`src/durable.c`): it creates a unique
sibling temporary file (`<path>.tmp.XXXXXX`) in the destination directory, writes all bytes handling
`EINTR` and short writes, sets permissions with `fchmod`, flushes with `fsync`, closes the descriptor,
and atomically renames the temporary file over `<path>`. On any error before `rename` succeeds, the
temporary file is unlinked and any existing destination file remains untouched. `write_file()` never
opens the live destination path with `O_TRUNC`.
Check: test (keep bar 1: filesystem durability boundary; keep bar 2: data safety). Test:
`test/sysstate_test.c`; compile boundary `test/sdkcheck.sh`.
Source: `hazards/system-state` SYS-2, 2026-11-27.

## Non-goals

- Acquiring SQLite read locks on `/system_data/priv/mms/*.db` (forbidden by P4N-30).
- Making two-file system layouts (`*.rif` + `*.idx`, `d0.pdb` + `d1.pdb`) atomic across filenames on
  UFS; those remain documented hardware/system limitations in `docs/hazards/system-state.md`.
