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
  `REPACK (CONCURRENTLY)` as the unsafe rewriter. No query or `REPACK` is
  blocked yet.
- `pg_repack_safe_rewrites` record table, created by the extension.
- `pg_repack_safe.trace_storage_changes` setting (superuser), which reports
  each detected storage replacement as a NOTICE.
- Tests: `storage_change` regression test and
  `t/002_standby_record_removal.pl`.
