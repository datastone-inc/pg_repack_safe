# Changelog

All notable changes to pg_repack_safe will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Repository skeleton: PGXS build, extension control file, empty install
  script, and a `doc_examples` regression test that creates the extension.
- Loadable library with `object_access`, `ProcessUtility` and `ExecutorStart`
  hooks. It detects replacement of a table's storage and classifies
  `REPACK (CONCURRENTLY)` as the unsafe rewriter.
- With the library loaded, a query is refused when the extension exists but
  its record table cannot be found
  (renamed, moved to another schema, or dropped). The missing-table refusal
  is skipped in binary-upgrade mode so that `pg_upgrade` works.
- `REPACK (CONCURRENTLY)` is refused when the extension exists but its record
  table cannot be found, in binary-upgrade mode too.
- With the library loaded, `REPACK (CONCURRENTLY)` of a table whose storage
  has blocks records the table and its post-swap TOAST relation, with the
  rewriting transaction's ID, in `pg_repack_safe_rewrites`.
- Reader check: a query whose snapshot does not see a committed
  `REPACK (CONCURRENTLY)` of a table it reads fails with SQLSTATE 40001,
  "cannot read table ... with this snapshot", instead of reading the table
  as empty. Checked in the executor; `COPY ... TO` is not covered yet.
- `REPACK (CONCURRENTLY)` is refused in a database without the extension, and
  when the record table does not have the definition the extension created.
- `pg_repack_safe_rewrites` record table, created by the extension, always
  with the `heap` access method. `rewrite_xid` is a full transaction ID
  (`xid8`), so a record never comes to name a newer transaction.
- `pg_repack_safe.trace_storage_changes` setting (superuser), which reports
  each detected replacement of storage that existed before the current
  transaction as a NOTICE.
- Tests: `reader_guard_edges` isolation spec (edge cases and read paths of
  the reader guard), `storage_change`, `reader_check` and `writer`
  regression tests,
  `t/002_standby_record_removal.pl`, `t/003_pg_upgrade.pl`,
  `t/004_hook_chain.pl` (hook chaining with `pg_stat_statements`) and
  `t/005_reader_guard.pl` (the error as a client sees it) and
  `t/006_record_xid8.pl` (records across an XID epoch) and
  `t/007_standby_reader.pl` (the guard on a hot standby).
