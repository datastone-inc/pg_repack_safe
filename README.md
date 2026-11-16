<!-- markdownlint-disable MD033 MD041 -->
<div align="center">

<img src="assets/datastone-logo.svg" alt="dataStone Logo" width="400"/>

# pg_repack_safe

[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-19-blue.svg)](https://www.postgresql.org/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-C-orange.svg)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Extension](https://img.shields.io/badge/Type-PostgreSQL%20Extension-purple.svg)](https://www.postgresql.org/docs/current/extend-extensions.html)

Makes PostgreSQL 19's `REPACK (CONCURRENTLY)` safe: a query whose snapshot
predates the repack gets a retryable error instead of wrong rows.

</div>
<!-- markdownlint-enable MD033 MD041 -->

---

## The problem

PostgreSQL 19's `REPACK (CONCURRENTLY)` rewrites a table without blocking
reads or writes, but it is not MVCC-safe (documented in the `REPACK`
reference page and in "MVCC caveats"). After it commits, a transaction whose
snapshot was taken before it committed, and which had not yet touched the
table, reads the table as **empty**. Nothing raises an error. The same
happens to `pg_dump` (a dump with the table's rows missing), to cursors, and
to logical replication's initial table copy (a subscriber that never gets
the table's existing rows).

## What pg_repack_safe guarantees

With the library in `shared_preload_libraries` on the primary and on every
standby, and the extension created in the database:

- A read of a table, whose snapshot does not see a committed
  `REPACK (CONCURRENTLY)` of that table, fails with
  `ERROR: cannot read table "..." with this snapshot`, SQLSTATE 40001 (a
  serialization failure: retry the transaction), instead of returning wrong
  rows. This covers every query through the executor (views, functions,
  cursors, parallel plans, partitions, the TOAST table), `COPY ... TO`
  (including partitioned tables, `pg_dump`, and logical replication's
  initial copy), on the primary and on hot standbys.
- A standby without the library stops replaying at the first
  `REPACK (CONCURRENTLY)` instead of serving its rows.
- Reads that do not need an error get none: snapshots taken after the
  repack committed, snapshots that had already read the table (the repack
  waits for them), empty tables, `VACUUM FULL`, `CLUSTER`, plain `REPACK`.
  One accepted exception: a table whose only rows were inserted after the
  snapshot and that is then repacked gives a spurious error.

The reproducers that fail on stock PostgreSQL 19 and pass with the
extension are `specs/repack_concurrently_*.spec`, `t/001_repack_pg_dump.pl`,
`t/008_copy_guard.pl` and `t/013_tablesync.pl`.

## What it does not guarantee

See [Known gaps](#known-gaps): other commands that rewrite tables without
preserving visibility (`TRUNCATE`, rewriting `ALTER TABLE`, `REFRESH
MATERIALIZED VIEW`), the foreign-key check's error class, `VACUUM FULL`
against hot standbys without `hot_standby_feedback` (a stock PostgreSQL
problem), and a few configurations the extension cannot see.

## Requirements

- PostgreSQL 19, `wal_level = replica` or higher (required by
  `REPACK (CONCURRENTLY)` itself).
- `shared_preload_libraries = 'pg_repack_safe'` on the primary **and on
  every physical standby**, or anything else that replays the WAL.
  `REPACK (CONCURRENTLY)` is refused in a session that loaded the library
  any other way. A standby without it fails on the extension's WAL record
  and shuts down ("resource manager with ID 128 not registered").
- `CREATE EXTENSION pg_repack_safe` in every database where
  `REPACK (CONCURRENTLY)` runs; it is refused elsewhere.
- Recommended: `hot_standby_feedback = on` on hot standbys that run queries
  (see [Known gaps](#known-gaps)).

Details, including `pg_upgrade` and other extensions, are in
[doc/installation.md](doc/installation.md).

## How it works

Each `REPACK (CONCURRENTLY)` of a table with data writes a record, (table,
rewriting transaction ID), into the extension's table
`pg_repack_safe_rewrites`, plus one for the table's TOAST table, and a WAL
record of its own. Once a query holds its locks, it checks the records of
the tables it reads against its snapshot; each backend keeps a copy of the
records, refreshed by cache invalidation, so the check costs about as much
as a hash lookup. The design and its evidence are in
[doc/research/fix-design.md](doc/research/fix-design.md).

The record table is protected:

- Records are never deleted. Each `REPACK (CONCURRENTLY)` adds one or two
  rows; the table only grows.
- While it holds records, `DROP` of the table, its triggers or the
  extension, and `TRUNCATE` of the table, are refused unless a superuser
  sets `pg_repack_safe.allow_unprotected_drop` (see
  [Removing the extension](#removing-the-extension)). `UPDATE` and `DELETE`
  of records, rewrites that do not keep every record unchanged (a rewriting
  `ALTER TABLE`, `REPACK (CONCURRENTLY)`), and changes to when its triggers
  fire are always refused. `VACUUM`, `VACUUM FULL`, `CLUSTER` and `REPACK`
  of it, also database-wide, are allowed.
- If the record table loses its definition or either trigger by a route no
  hook could stop, every query and `REPACK (CONCURRENTLY)` in that database
  is refused until the extension is re-created.

## Installation

```bash
make
sudo make install        # omit sudo when the PostgreSQL install is yours
```

Then add the library to `shared_preload_libraries` (primary and standbys),
restart, and in each database:

```sql
CREATE EXTENSION pg_repack_safe;
```

See [doc/installation.md](doc/installation.md).

## Known gaps

Each has a test that pins today's behaviour.

- **TRUNCATE and rewriting ALTER TABLE** (for example a column type change)
  are not recorded: a snapshot taken before them reads the table as empty,
  as in stock PostgreSQL, where both are documented as not MVCC-safe
  (`specs/known_gaps.spec`).
- **REFRESH MATERIALIZED VIEW** without `CONCURRENTLY` is not recorded: a
  snapshot taken before it reads the view's *new* contents, because the
  refresh writes its rows frozen. `REFRESH ... CONCURRENTLY` is not affected
  (`specs/known_gaps.spec`).
- **Foreign-key checks** use a path that bypasses the reader check: an
  INSERT or UPDATE in a transaction whose snapshot predates a
  `REPACK (CONCURRENTLY)` of the referenced table fails with a foreign-key
  violation (SQLSTATE 23503) instead of a serialization failure (40001). No
  wrong rows, but an error a client would not retry
  (`specs/known_gaps.spec`).
- **Hot standbys without `hot_standby_feedback`**: `VACUUM FULL`, `CLUSTER`
  and plain `REPACK` on the primary can drop rows that a standby snapshot
  still needs, and the standby query then silently misses them, with or
  without this extension (stock PostgreSQL behaviour, reported on
  pgsql-hackers in 2012 and still unfixed). Turn on `hot_standby_feedback`,
  ideally with a physical replication slot, on every hot standby that runs
  queries (`t/012_standby_safe_rewrite.pl`). One standby with feedback holds
  the primary's cleanup back for every standby. If users ask for it, this
  extension could also guard these rewrites on standbys, as it does
  `REPACK (CONCURRENTLY)`.
- **A standby without the library** whose replay starts in the moment
  between a `REPACK (CONCURRENTLY)`'s WAL record and its commit, and **a
  session that never loads the library**: see the requirements above.

Covered, for the record: `pg_dump`, `COPY ... TO`, logical replication's
initial table copy (refused, and the sync worker retries;
`t/013_tablesync.pl`), and PL/pgSQL procedures that commit inside a loop
(`t/014_plpgsql_toast.pl`).

## Optional pre-check

The reader guard turns a wrong result into an error, which a long report or
transaction may still not want: it fails, and its work is lost. With
`pg_repack_safe.precheck_snapshot_age` set (off by default, `-1`),
`REPACK (CONCURRENTLY)` instead refuses to run, at its start and again just
before it swaps the table, while a session in the same database could later
read the table with an older snapshot:

```sql
SET pg_repack_safe.precheck_snapshot_age = '10min';
```

A session counts when it holds a snapshot that outlives a statement (idle
in a transaction, or past the first statement of its transaction), holds no
lock on the table (`REPACK` waits for those), and its transaction has been
open at least that long (`0`: any age). A single statement, however long,
does not count: it locked every table it reads when it started.
`pg_dump` locks every table it dumps right after taking its snapshot, so a
running dump is waited for, not counted.

This check is best-effort, not part of the guarantee. It does not see
standbys, background workers, or snapshots taken after it runs, and since
it cannot see a session's isolation level, it also counts later statements
of a `READ COMMITTED` transaction, which would not have been hurt.

## Removing the extension

Records protect snapshots taken before a `REPACK (CONCURRENTLY)`, on the
primary and on every standby. Dropping the extension removes that
protection, so it is refused while records exist. To remove it safely, in
this order:

1. Stop running `REPACK (CONCURRENTLY)`. A new one adds a newer record.
2. In each database with the extension, on the primary **and on every
   standby**, wait until this query returns no rows. It lists sessions whose
   snapshot may predate the newest record (on any database, so it errs
   towards waiting):

   ```sql
   SELECT pid, datname, backend_xmin
   FROM pg_stat_activity
   WHERE backend_xmin IS NOT NULL
     AND age(backend_xmin) >= (SELECT age(max(rewrite_xid)::xid)
                               FROM pg_repack_safe_rewrites);
   ```

3. Drop the extension in each database, with the override:

   ```sql
   SET pg_repack_safe.allow_unprotected_drop = on;
   DROP EXTENSION pg_repack_safe;
   RESET pg_repack_safe.allow_unprotected_drop;
   ```

4. Only then remove `pg_repack_safe` from `shared_preload_libraries` and
   restart, the primary first, then each standby once it has replayed the
   drops. While the extension exists, a server without the library
   preloaded runs `REPACK (CONCURRENTLY)` unrecorded and checks no reads.

## Documentation

- [Installation Guide](doc/installation.md)
- [Development Guide](doc/development.md)

## License

MIT; see [LICENSE](LICENSE).
