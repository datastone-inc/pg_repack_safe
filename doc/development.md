# Development Guide

## Prerequisites

- PostgreSQL 19 with development headers
- GCC or Clang with C99 support
- Git
- Make

`make installcheck` also needs:

- a PostgreSQL 19 build configured with TAP tests enabled
  (`--enable-tap-tests`), and the Perl module `IPC::Run`;
- the contrib modules `pg_stat_statements` (`t/004_hook_chain.pl`) and
  `pageinspect` (`t/002_standby_record_removal.pl`) installed.

## Clone and Build

```bash
git clone https://github.com/datastone-inc/pg_repack_safe.git
cd pg_repack_safe
make
```

## Testing

Regression tests use `pg_regress` through PGXS. `sql/` holds the test
scripts and `expected/` the output each must produce.

```bash
make install
make installcheck
```

A failing run leaves `regression.diffs` in the repository root.

`make installcheck` runs three suites. None of them uses the server that
`PGHOST`/`PGPORT` point at; each starts its own instances from the installed
binaries.

- `sql/` + `expected/` (`REGRESS`): single-session tests, on a temporary
  instance under `tmp_check_regress/`.
- `specs/*.spec` + `expected/*.out` (`ISOLATION`): concurrency
  interleavings, on a temporary instance under `tmp_check_iso/`. A failing
  run leaves its diffs under `output_iso/`.
- `t/*.pl` (`TAP_TESTS`): cases that need their own server setup or separate
  client programs such as `pg_dump`. Each creates its own clusters under
  `tmp_check/`. Helpers they share (a preloaded cluster, a session holding
  an old snapshot, running a statement and collecting its errors, waiting
  for a queued lock request) are in `t/RepackSafeTest.pm`.

The `REGRESS` and `ISOLATION` instances get the settings in
`test/temp_instance.conf`: the library in `shared_preload_libraries`, which
the reader guard needs and without which `REPACK (CONCURRENTLY)` is refused,
and `wal_level = replica`, which `REPACK (CONCURRENTLY)` needs. A failing run
leaves its instance in place for inspection.

`pg_config` on `PATH` (or `PG_CONFIG`) must point at PostgreSQL 19:

```bash
make PG_CONFIG=/path/to/pg19/bin/pg_config install installcheck
```

Every SQL example in the documentation has a matching test in
`sql/doc_examples.sql` (see `project_template.md`).
