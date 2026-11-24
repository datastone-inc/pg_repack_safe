# Development Guide

## Prerequisites

- PostgreSQL 19 with development headers
- GCC or Clang with C99 support
- Git
- Make
- clang-format 22, for `make check-format` and `make format`

`make installcheck` also needs:

- a PostgreSQL 19 build configured with TAP tests enabled
  (`--enable-tap-tests`), and the Perl module `IPC::Run`;
- the contrib modules `pg_stat_statements` (`t/004_hook_chain.pl`),
  `pageinspect` (`t/002_standby_record_removal.pl`,
  `t/003_pg_upgrade.pl`, `t/017_standby_record_cleanup.pl`,
  `sql/writer.sql`, `sql/records_prune.sql`), and `pgrowlocks` and
  `amcheck` (`t/019_contrib_readers.pl`) installed.

## Clone and Build

```bash
git clone https://github.com/datastone-inc/pg_repack_safe.git
cd pg_repack_safe
make
```

The build enables `-Wall -Wextra`. `make WERROR=1` also turns warnings into
errors, in the library and in the test modules; use it before sending a
change.

## Formatting

The C files follow `.clang-format`. `make check-format` changes nothing and
fails if a file is not formatted; `make format` rewrites the files in
place. Output differs between clang-format majors, so name the binary if
the default `clang-format` is another major:

```bash
make check-format CLANG_FORMAT=clang-format-22
```

`pipx install clang-format==22.1.8` installs the version the CI uses.

## Testing

Regression tests use `pg_regress` through PGXS. `sql/` holds the test
scripts and `expected/` the output each must produce.

```bash
make install
make installcheck
```

A failing run leaves `regression.diffs` in the repository root.

## Demo

`demo/demo.sh` creates a throwaway server in a temporary directory and runs
the scenarios `demo/[0-9]_*.sql` against it, first without the library and
then with the extension. Each scenario is one psql script that runs the
other session's statements with `\!`, so the interleaving is fixed.
`t/018_demo.pl` runs the demo and checks both halves of its output, which
also makes it the one test of what stock PostgreSQL 19 does in these cases.

## Continuous Integration

`.github/workflows/ci.yml` runs on every push and pull request:

- `make check-format`, with clang-format 22.1.8.
- PostgreSQL 19 from the apt.postgresql.org packages, built with gcc on
  x86_64 and arm64, with `make WERROR=1`, then `make installcheck`.

Weekly, and on request, it also builds the tip of PostgreSQL's
`REL_19_STABLE` branch from source with clang, assertions and injection
points, and runs the suites against it. The extension relies on server
internals, and this is where a change in them shows up first.

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

`make installcheck` first builds four test-only modules in `test/modules/`.
They are never installed: the Makefile writes
`test/modules/library_path.conf`, a `dynamic_library_path` that names their
directories, and every temporary server gets it (pg_regress and
pg_isolation_regress as a second `--temp-config`, the TAP clusters through
`TEMP_CONFIG`). `test_record` can write arbitrary rows into a record table,
which is why it must not reach a server's library directory. A
`TEMP_CONFIG` set in the environment is included from that file, so its
settings reach the regress, isolation and TAP servers alike:

```bash
TEMP_CONFIG=~/debug.conf make installcheck
```

- `custom_scan_provider`: plans every scan of a plain table as a
  `CustomScan` while `custom_scan_provider.enabled` is on.
  `specs/unread_relations.spec` loads it to check that the reader check
  covers every range-table relation when a plan has a `CustomScan`.
- `oat_observer`: a second `object_access_hook`, loaded next to the library
  by `t/015_oat_hook_chain.pl`.
- `test_record`: the C function `test_insert_record(record_table regclass,
  relid oid, rewrite_xid xid8)`, which each test that uses it creates with
  `CREATE FUNCTION ... AS 'test_record' LANGUAGE C`. The extension refuses
  every insert into its record table except the one `REPACK (CONCURRENTLY)`
  makes, and that one always records its own transaction. Tests of the
  reader check need records with other transaction IDs (aborted, from
  another epoch, not yet assigned), and this function writes them much as
  the extension's writer does: `simple_heap_insert`, which fires no trigger,
  and a relcache invalidation of the record table. Unlike the writer's, its
  insert is visible to logical decoding, which `t/016_publication.pl` uses
  to send a record to a subscriber. Also used by `sql/reader_check.sql`,
  `sql/records_prune.sql`, `specs/reader_cache.spec` and
  `t/006_record_xid8.pl`.
- `toplevel_worker`: the C function `test_run_in_worker(sql text)`, created
  the same way, which runs a statement at top level in a dynamic background
  worker and returns NULL, or the error message. `REPACK (CONCURRENTLY)`
  cannot run through SPI, so the worker runs it as `exec_simple_query`
  does. A background worker never loads `session_preload_libraries`;
  `t/009_not_preloaded.pl` uses this to pin that gap.

The `REGRESS` and `ISOLATION` instances get the settings in
`test/temp_instance.conf`: the library in `shared_preload_libraries`, which
the reader guard needs and without which `REPACK (CONCURRENTLY)` is refused,
and `wal_level = replica`, which `REPACK (CONCURRENTLY)` needs. A failing run
leaves its instance in place for inspection.

`pg_config` on `PATH` (or `PG_CONFIG`) must point at PostgreSQL 19:

```bash
make PG_CONFIG=/path/to/pg19/bin/pg_config install installcheck
```

## Releasing

The library's version is `PG_REPACK_SAFE_VERSION` in
`src/pg_repack_safe.c`, in semantic-versioning form. `META.json` carries
the same version for PGXN, and the control file's `default_version` is its
first two parts. `make check-version`, which CI runs, fails when the three
disagree.

- A release that changes only C code changes the third part (`1.0.1`). The
  extension's version stays `1.0`, and nobody runs `ALTER EXTENSION
  UPDATE`.
- A release that changes the SQL objects changes the second or first part
  and the extension's version with it, and ships an upgrade script
  (`pg_repack_safe--1.0--1.1.sql`) with a test of the upgrade.
- The custom WAL record's format (`xl_pg_repack_safe_rewrite`) and its
  resource manager ID do not change within a major version of the library
  (its first part): during a rolling upgrade the primary and its standbys
  run different builds.
- A library supports every extension version of its major version: it
  reads and writes the record table of `1.0` as well as that of `1.1`. A
  rolling upgrade installs the new build on the standbys first and runs
  `ALTER EXTENSION UPDATE` last, so a standby may meet the SQL objects of
  any earlier extension version of the major, never a later one. A change
  that cannot keep this changes the first part.

To release: move the `[Unreleased]` entries of `CHANGELOG.md` under the new
version and date, set the three versions, commit, then

```bash
make check-version
git tag -s v1.0.0 -m 'pg_repack_safe 1.0.0'
make dist                # pg_repack_safe-1.0.0.zip, from the committed tree
```

and upload the archive to PGXN. A released install script
(`pg_repack_safe--1.0.sql`) is never edited: a change to the SQL objects
goes into the next extension version's script and its upgrade script.

The archive is `git archive` of the committed tree, less the files
`.gitattributes` marks `export-ignore`: the instructions for coding agents
(`AGENTS.md`, `CLAUDE.md`, `.claude/`). PGXN lists every Markdown file in
the archive as documentation, except those under `no_index` in
`META.json`: `doc/research/` stays in the archive, because the user docs
link to it, but not in that list.

## Documentation layout

`README.md` is the first screen: the problem, what the extension does, how
to install and verify it, what it costs, what it guarantees and what it
does not. Everything longer is in `doc/`:

- `problem.md`: the problem in detail and the demo's output;
- `installation.md`: requirements, settings, upgrades, logical replication;
- `design.md`: the argument that the guard is sound, with the function, the
  PostgreSQL source and the test behind each step, and the costs;
- `limits.md`: errors where a result might have been correct, known gaps,
  and what is lost when a requirement is not met;
- `standbys.md`: recovery conflicts on standbys without
  `hot_standby_feedback`;
- `removal.md`: removing the extension without exposing a snapshot;
- `research/`: the notes and history the design came from.

A change to a hook or to what a check covers updates `design.md` in the
same commit.

Every SQL example in the documentation has a matching test in
`sql/doc_examples.sql`, headed by the document and section it comes from.
