# Development Guide

## Prerequisites

- PostgreSQL 19 with development headers
- GCC or Clang with C99 support
- Git
- Make

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

`make installcheck` runs three suites:

- `sql/` + `expected/` (`REGRESS`): single-session tests.
- `specs/*.spec` + `expected/*.out` (`ISOLATION`): concurrency
  interleavings. They run on a temporary instance under `tmp_check_iso/`,
  built from the installed binaries with the settings in
  `specs/isolation.conf`: the library in `shared_preload_libraries`, which the
  reader guard needs, and `wal_level = replica`, which
  `REPACK (CONCURRENTLY)` needs. The server that `PGHOST`/`PGPORT` point at is
  not used. A failing run leaves its diffs under `output_iso/` and the
  instance in `tmp_check_iso/`.
- `t/*.pl` (`TAP_TESTS`): cases that drive separate client programs such as
  `pg_dump`. Each creates its own temporary cluster under `tmp_check/`.

`pg_config` on `PATH` (or `PG_CONFIG`) must point at PostgreSQL 19:

```bash
make PG_CONFIG=/path/to/pg19/bin/pg_config install installcheck
```

Every SQL example in the documentation has a matching test in
`sql/doc_examples.sql` (see `project_template.md`).
