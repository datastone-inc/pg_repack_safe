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
  and shuts down ("resource manager with ID 149 not registered").
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

`COPY partitioned_table TO` is run as `COPY (SELECT columns FROM
partitioned_table) TO`, in every database of a server with the library
loaded, so that the check covers exactly the partitions that are read. The
rows and their format are the same and no extra lock is taken. Three things
differ from stock PostgreSQL 19:

- a partition that is a foreign table is read, where stock raises "cannot
  copy from foreign table";
- the statement goes through the executor, which measured about 5% slower
  (4 million rows in 8 partitions, text and binary);
- a partitioned table with row-level security is unchanged: PostgreSQL
  already runs it as a query.

Logical replication's initial copy of a table published through its
partitioned root is such a statement; `pg_dump` copies each partition by
itself and is not affected.

A publication `FOR ALL TABLES` includes `pg_repack_safe_rewrites`. Exclude
it: a subscriber that has the extension refuses the publisher's records,
and its subscription then applies nothing more until the table is excluded
and the subscription refreshed (`t/016_publication.pl`; see "Logical
Replication" in [doc/installation.md](doc/installation.md)).

The record table is protected:

- Each `REPACK (CONCURRENTLY)` adds one or two records and deletes every
  older one, so the table stays small. A deleted record still protects: it
  stays in the table until `VACUUM` finds that no snapshot can need it.
  `pg_repack_safe_prune_records()` deletes the remaining records in a
  database that has stopped repacking.
- While it holds records, `DROP` of the table, its triggers or the
  extension, and `TRUNCATE` of the table, are refused unless a superuser
  sets `pg_repack_safe.allow_unprotected_drop` (see
  [Removing the extension](#removing-the-extension)). `UPDATE` and `DELETE`
  of records, rewrites that do not keep every record unchanged (a rewriting
  `ALTER TABLE`, `REPACK (CONCURRENTLY)`), and changes to when its triggers
  fire are always refused. `VACUUM` of it is always allowed.
- `VACUUM FULL`, `CLUSTER` and `REPACK` of it are refused while it holds a
  deleted record, because they would drop the record without ending the
  standby snapshots that still need it. Run `VACUUM
  pg_repack_safe_rewrites` first and repeat the command. The database-wide
  `VACUUM FULL` and `REPACK` stop at the record table with the same error
  and the same remedy.
- `INSERT` into it is always refused, by any route (`COPY ... FROM`,
  `MERGE`, logical replication) and whatever `session_replication_role` is.
  Only `REPACK (CONCURRENTLY)` writes records. A record names a table by
  OID and a transaction by ID, which mean something only in the cluster
  that wrote it; a row from anywhere else would refuse reads of whichever
  table has that OID.
- Creating an index on it, by `CREATE INDEX` or a `PRIMARY KEY` or `UNIQUE`
  constraint, is always refused. Records are inserted without maintaining
  indexes, so an index would miss every later record and a query using it
  would return stale results.
- Adding, dropping or altering a column of it (the type, default, name or
  any other property) is always refused. With any other column list, every
  query in that database would be refused.
- If the record table loses its definition or any of its three triggers by
  a route no hook could stop, every query and `REPACK (CONCURRENTLY)` in that database
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
- **Old snapshots on a hot standby without `hot_standby_feedback` are
  ended** when the primary vacuums the record table after a
  `REPACK (CONCURRENTLY)`, whether or not they would have read a repacked
  table: a running query is cancelled with a recovery-conflict error
  (SQLSTATE 40001), and a session idle in a transaction is terminated. This
  is how a deleted record is kept from disappearing under a standby
  snapshot that needs it, and it affects every transaction in that database
  whose snapshot is older than the latest `REPACK (CONCURRENTLY)`. Autovacuum
  vacuums the record table once enough records have been deleted (about 50
  with its default settings); a manual `VACUUM` of the record table causes
  it at once. With
  `hot_standby_feedback` on, the primary keeps the records instead
  (`t/007_standby_reader.pl`, `t/017_standby_record_cleanup.pl`).
- **A standby without the library** whose replay starts in the moment
  between a `REPACK (CONCURRENTLY)`'s WAL record and its commit: see
  [Requirements](#requirements).
- **A session that never loads the library** has no hooks: its
  `REPACK (CONCURRENTLY)` runs unrecorded and its reads are not checked
  (`t/009_not_preloaded.pl`). Setting `shared_preload_libraries`, as the
  requirements say, leaves no such session.

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
protection, so it is refused while the record table holds any record,
deleted ones included. To remove it safely, in this order:

1. Stop running `REPACK (CONCURRENTLY)`. A new one adds a newer record.
2. In each database with the extension, on the primary, delete the
   remaining records:

   ```sql
   SELECT pg_repack_safe_prune_records();
   ```

3. Vacuum the record table until it has no blocks:

   ```sql
   VACUUM pg_repack_safe_rewrites;
   SELECT pg_relation_size('pg_repack_safe_rewrites') = 0 AS empty;
   ```

   `VACUUM` keeps a deleted record for as long as a snapshot on the primary
   could need it, so repeat this once older transactions have ended. On a
   standby, replaying the removal ends the snapshots that needed the record
   (see [Known gaps](#known-gaps)), so there is nothing to check there.
4. Drop the extension in each database. With the record table empty this
   needs no override:

   ```sql
   DROP EXTENSION pg_repack_safe;
   ```

5. Only then remove `pg_repack_safe` from `shared_preload_libraries` and
   restart, the primary first, then each standby once it has replayed the
   drops. While the extension exists, a server without the library
   preloaded runs `REPACK (CONCURRENTLY)` unrecorded and checks no reads.

A superuser can skip steps 2 and 3 by setting
`pg_repack_safe.allow_unprotected_drop`. A snapshot taken before a recorded
`REPACK (CONCURRENTLY)`, on the primary or on any standby, then reads the
repacked table as empty, so this is for a server where no such snapshot can
exist.

## Documentation

- [Installation Guide](doc/installation.md)
- [Development Guide](doc/development.md)

## License

MIT; see [LICENSE](LICENSE).
