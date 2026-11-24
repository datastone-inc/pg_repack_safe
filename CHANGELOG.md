# Changelog

All notable changes to pg_repack_safe will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- The release archive leaves out the instructions for coding agents
  (`AGENTS.md`, `CLAUDE.md`, `.claude/`), and PGXN no longer lists
  `doc/research/` among the extension's documentation.

## [1.0.0] - 2026-11-24

### Added

- The `pg_repack_safe` extension and library for PostgreSQL 19, built with
  PGXS. The library is loaded through `shared_preload_libraries` on the
  primary and on every standby, and installs `object_access`,
  `ProcessUtility` and `ExecutorStart` hooks, each chained to the previous
  one.
- The reader guard. A read of a table whose snapshot does not see a
  committed `REPACK (CONCURRENTLY)` of that table fails with SQLSTATE
  40001, `cannot read table "..." with this snapshot`, instead of reading
  the table as empty. It covers every query through the executor (views,
  functions, cursors, parallel plans, partitions, the TOAST table), the
  reads an `UPDATE` or `DELETE` makes to find its rows, `COPY ... TO`,
  `pg_dump` and logical replication's initial table copy, on the primary
  and on hot standbys. An `INSERT`'s target table is not checked unless it
  has a foreign key to itself, and a table the plan names but does not
  read is not checked. A read without an MVCC snapshot of a table that has
  a record is refused. Each backend keeps a copy of the records, reloaded
  when `REPACK (CONCURRENTLY)` invalidates it.
- `COPY partitioned_table TO` runs as `COPY (SELECT columns FROM
  partitioned_table) TO`, so that a partition attached during the `COPY` is
  checked too. The output is the same; a partition that is a foreign table
  is read where stock PostgreSQL raises an error, and the statement is
  about 5% slower.
- The record table `repack_safe.rewrites`. `REPACK (CONCURRENTLY)` of a
  table whose storage has blocks records the table and its TOAST table,
  with the rewriting transaction's full ID (`xid8`), and deletes every
  older record. `repack_safe.prune_records()` deletes them on request. A
  deleted record still protects until `VACUUM` finds that no snapshot can
  need it; on a hot standby, replaying its removal ends the snapshots that
  still do.
- `REPACK (CONCURRENTLY)` is refused when the library was not loaded
  through `shared_preload_libraries`, in a database without the extension,
  and when the record table is missing or does not have the definition the
  extension created. Every query in a database whose record table is
  missing, or has lost its definition or one of its two triggers, is
  refused, except in binary-upgrade mode and in the session running
  `CREATE EXTENSION`.
- The record table is protected. `INSERT` (by any route, logical
  replication included), `UPDATE` and `DELETE` of records, an index, a
  column change, a rewrite that does not keep every record and a change to
  when its triggers fire are always refused (SQLSTATE 0A000). While it
  holds records, `DROP` of the table, its triggers or the extension,
  `TRUNCATE`, renaming the table and moving it to another schema are
  refused unless a superuser sets `pg_repack_safe.allow_unprotected_removal`
  (SQLSTATE 55000). `VACUUM FULL`, `CLUSTER` and `REPACK` of it are refused
  while it holds a deleted record.
- The fixed schema `repack_safe`. The extension cannot be created in or
  moved to another schema. A schema `repack_safe` that already exists is
  used only if a superuser owns it. The extension requires `plpgsql`.
- Settings: `pg_repack_safe.precheck_snapshot_age` (superuser, off by
  default) makes `REPACK (CONCURRENTLY)` refuse to run, at its start and at
  its swap, while a session in the database holds an old snapshot that
  could later read the table; `pg_repack_safe.allow_unprotected_removal`
  (superuser) lifts the removal refusals above;
  `pg_repack_safe.trace_storage_changes` (superuser) reports each replaced
  table storage as a `NOTICE`.
- `repack_safe.status()` returns true where the guard is in force, and
  false with a `WARNING` where the server did not load the library through
  `shared_preload_libraries`. A session that loads the library later gets
  the same `WARNING`, which names `session_preload_libraries` or
  `local_preload_libraries` when the library is listed there. `\dx` shows
  the check in the extension's comment.
- A custom WAL resource manager (ID 149): each `REPACK (CONCURRENTLY)`
  writes a record before its commit, so a server that replays the WAL
  without the library stops instead of serving the repacked table to old
  snapshots.
- The library's version, `1.0.0`, shown by `pg_get_loaded_modules()` and
  logged by the postmaster at start; the extension's version is `1.0`. The
  primary and its standbys need the same major version of the library; an
  upgrade installs the new build on the standbys first.
- Logical replication: the records `REPACK (CONCURRENTLY)` writes are not
  sent to subscribers. A new subscription's initial copy reads the records
  already there, which a subscriber with the extension refuses until the
  record table is excluded from the publication (`doc/installation.md`,
  "Logical Replication").
- `pg_upgrade` carries the records over; `pg_dump` leaves them behind.
- `demo/demo.sh`: a report, an `UPDATE`, a `DELETE` and a `pg_dump` on a
  throwaway server, without and then with the extension.
- Packaging: `META.json` for PGXN, `make dist`, and `make check-version`.
  `make installcheck` runs 8 regression, 9 isolation and 22 TAP files on
  temporary servers, with test-only modules loaded from the build tree.
- Documentation: the README, and in `doc/` the problem in detail
  (`problem.md`), installation and settings (`installation.md`), the design
  argument (`design.md`), limits (`limits.md`), standby errors
  (`standbys.md`), removal (`removal.md`) and development
  (`development.md`).

### Known gaps

Each is described in `doc/limits.md`:

- Functions of `pgrowlocks` and `amcheck` read a table outside the executor
  and return a wrong answer, with no error, to an old snapshot.
- A foreign-key check against a repacked table from an old snapshot fails
  with 23503, not 40001.
- A standby without the library whose replay starts between a repack's
  swap and its commit is not stopped.

[Unreleased]: https://github.com/datastone-inc/pg_repack_safe/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/datastone-inc/pg_repack_safe/releases/tag/v1.0.0
