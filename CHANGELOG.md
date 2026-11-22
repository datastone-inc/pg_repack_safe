# Changelog

All notable changes to pg_repack_safe will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `pg_repack_safe` extension and library for PostgreSQL 19, built with PGXS.
  The library installs `object_access`, `ProcessUtility` and `ExecutorStart`
  hooks, each chained to the previous one. It detects replacement of a
  table's storage and classifies `REPACK (CONCURRENTLY)` as the unsafe
  rewriter. It builds against PostgreSQL with or without assertions.
- `pg_repack_safe_rewrites` record table, created by the extension, always
  with the `heap` access method. `REPACK (CONCURRENTLY)` of a table whose
  storage has blocks records the table and its post-swap TOAST relation
  there, with the rewriting transaction's full ID (`xid8`), so a record
  never comes to name a newer transaction.
- Reader check: a query whose snapshot does not see a committed
  `REPACK (CONCURRENTLY)` of a table it reads fails with SQLSTATE 40001,
  "cannot read table ... with this snapshot", instead of reading the table
  as empty. Checked in the executor and for `COPY relation TO` (including
  partitioned tables), so `pg_dump` fails instead of dumping no rows. Each
  backend keeps a copy of the records, so a query does not read the record
  table; the writer, and a statement trigger on the record table for changes
  made by hand, invalidate it.
- `COPY partitioned_table TO` is run as `COPY (SELECT columns FROM
  partitioned_table) TO`, so that a partition attached during the COPY
  cannot be scanned unchecked and no lock beyond COPY's own is taken. The
  output is unchanged; a partition that is a foreign table is read where
  stock PostgreSQL raises an error, and the statement is about 5% slower.
- `REPACK (CONCURRENTLY)` is refused unless the library was loaded through
  `shared_preload_libraries`, in a database without the extension, and when
  the record table cannot be found or does not have the definition the
  extension created.
- A query is refused when the extension exists but its record table cannot
  be found (renamed, moved to another schema, or dropped) or lacks any of
  its three triggers firing ALWAYS. Both refusals are skipped in binary-upgrade
  mode so that `pg_upgrade` works; `REPACK (CONCURRENTLY)` stays refused
  there. `pg_upgrade` carries the records over unchanged; `pg_dump` leaves
  them behind.
- The record table is protected. `INSERT` (by any route: `COPY ... FROM`,
  `MERGE`, logical replication), `UPDATE` and `DELETE` of records,
  `REPACK (CONCURRENTLY)` and rewriting `ALTER TABLE` of the table, and
  changes to when its triggers fire are refused (`VACUUM FULL`, `CLUSTER`
  and `REPACK` are allowed). Creating an index on it, which the writer
  would not maintain, and adding, dropping or altering one of its columns,
  which would make every query in the database fail, are refused too. While
  it holds records, `DROP` of the table, its triggers or the extension, and
  `TRUNCATE`, are refused unless a superuser sets
  `pg_repack_safe.allow_unprotected_drop`. The README
  describes how to remove the extension safely.
- Only `REPACK (CONCURRENTLY)` writes records. A row-level
  `BEFORE INSERT` trigger on the record table, enabled ALWAYS, refuses every
  other insert with "cannot insert into the pg_repack_safe record table".
  A publication `FOR ALL TABLES` includes the record table, and without the
  refusal a subscriber with the extension stored the publisher's records,
  which took the subscriber's table with the same OID offline. The refusal
  stops the subscription instead, until the table is excluded from the
  publication and the subscription is refreshed; `doc/installation.md`,
  "Logical Replication", gives the steps.
- Custom WAL resource manager (ID 149, reserved on the PostgreSQL wiki): each
  `REPACK (CONCURRENTLY)` logs a record before its commit, so a standby
  without the library stops replay instead of serving the repacked table to
  old snapshots.
- Optional pre-check, `pg_repack_safe.precheck_snapshot_age` (off by
  default): `REPACK (CONCURRENTLY)` refuses to run, at its start and at the
  swap, while a session in the database holds a snapshot that outlives a
  statement, has not locked the table, and is at least that old.
- `pg_repack_safe.trace_storage_changes` setting (superuser), which reports
  each detected replacement of storage that existed before the current
  transaction as a NOTICE.
- Documentation: the README covers what is unsafe in stock PostgreSQL 19,
  what the extension guarantees and what it does not; `doc/installation.md`
  covers server and standby configuration, settings, `pg_upgrade` and other
  extensions.
- Tests, run by `make installcheck` on temporary instances:
  - regression: `doc_examples`, `storage_change`, `reader_check`, `writer`,
    `protection`, `copy_partitioned`;
  - isolation: `repack_concurrently_snapshot`, `repack_concurrently_cursor`,
    `reader_guard_edges`, `reader_cache`, `precheck`, `copy_attach_race`, and
    `known_gaps`, which pins what the extension does not cover (TRUNCATE,
    rewriting ALTER TABLE, REFRESH MATERIALIZED VIEW, the foreign-key
    check);
  - TAP (`t/001` to `t/016`): `pg_dump`, `pg_upgrade`, hot standbys, hook
    chaining with `pg_stat_statements` and with the test-only module
    `test/modules/oat_observer`, COPY, the pre-check, logical replication's
    initial copy, PL/pgSQL procedures that commit in a loop, and a
    publication that includes the record table.
  - Test-only modules `test/modules/oat_observer` and
    `test/modules/test_record` (writes a record with a chosen table and
    transaction ID), installed by `make installcheck`, not by
    `make install`.
