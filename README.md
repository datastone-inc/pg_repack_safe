<!-- markdownlint-disable MD033 MD041 -->
<div align="center">

<img src="assets/datastone-logo.svg" alt="dataStone Logo" width="400"/>

# pg_repack_safe

[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-19-blue.svg)](https://www.postgresql.org/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-C-orange.svg)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Extension](https://img.shields.io/badge/Type-PostgreSQL%20Extension-purple.svg)](https://www.postgresql.org/docs/current/extend-extensions.html)

PostgreSQL extension (description to come)

</div>
<!-- markdownlint-enable MD033 MD041 -->

---

## Overview

The extension is under construction. With the library in
`shared_preload_libraries` and the extension created, it prevents the
documented wrong result for queries: a query whose snapshot predates a
committed `REPACK (CONCURRENTLY)` of a table it reads gets an error instead
of an empty table, on the primary and on a hot standby that also has the
library preloaded. That covers queries, `COPY ... TO` and so `pg_dump`. It
does **not** yet cover readers that bypass both the executor and COPY, or
the other cases listed in
[doc/research/fix-design.md](doc/research/fix-design.md).

Requirements and refusals:

- The library must be in `shared_preload_libraries`. `REPACK (CONCURRENTLY)`
  is refused in a session that loaded it any other way, and in a database
  without the extension. A session that never loads the library cannot be
  stopped: its `REPACK (CONCURRENTLY)` runs unrecorded (a known gap).
- **Every physical standby, and anything else that replays this server's
  WAL, needs the library in `shared_preload_libraries` too.**
  `REPACK (CONCURRENTLY)` writes a WAL record of its own before it commits;
  a standby without the library fails on that record and shuts down
  ("resource manager with ID 128 not registered") instead of serving the
  repacked table to old snapshots. Known gap: a standby without the library
  whose replay starts from a checkpoint taken between that record and the
  commit (for example one built from a base backup started at that moment)
  never sees the record.
- Records are never deleted. Each `REPACK (CONCURRENTLY)` of a table with
  data adds one or two rows to `pg_repack_safe_rewrites`. Each backend keeps
  a copy of them and reloads it when the table changes, so a growing table
  costs memory and reload time, not a scan per query.
- Because old snapshots, on the primary or a standby, may rely on any
  record, the record table is protected. While it holds records, `DROP` of
  the table, its triggers or the extension, and `TRUNCATE` of the table,
  are refused unless a superuser sets `pg_repack_safe.allow_unprotected_drop`
  (see [Removing the extension](#removing-the-extension)). `UPDATE` and
  `DELETE` of records, rewrites of the table that do not keep every record
  unchanged (a rewriting `ALTER TABLE`, `REPACK (CONCURRENTLY)`), and changes
  to when its triggers fire are always refused. `VACUUM`, `VACUUM FULL`,
  `CLUSTER` and `REPACK` of it, also database-wide, are allowed.
- If the record table loses its definition or either trigger by a route no
  hook could stop, every query and `REPACK (CONCURRENTLY)` in that database
  is refused until the extension is re-created.

What exists so far:

- a loadable library whose hooks detect when a table's storage is replaced
  and whether `REPACK (CONCURRENTLY)` did it;
- the writer: with the library loaded, `REPACK (CONCURRENTLY)` of a table
  that has data writes a record, (table, rewriting transaction ID), into the
  extension's table `pg_repack_safe_rewrites`, plus one for the table's TOAST
  relation. `REPACK (CONCURRENTLY)` is refused in a database where the
  extension is not created, and when its record table cannot be found or has
  been redefined;
- the reader check: once a query holds its locks, it raises
  `ERROR: cannot read table "..." with this snapshot` (SQLSTATE 40001, a
  serialization failure: retry the transaction) when a table it reads has a
  record whose `REPACK (CONCURRENTLY)` committed but is not visible to the
  query's snapshot. `EXPLAIN` without `ANALYZE` is not checked, since it
  reads no rows. Queries are also refused if the extension exists but the
  record table has been renamed, moved to another schema, or dropped. That second refusal is off while the server runs in
  binary-upgrade mode (`pg_upgrade`, or a server started by hand with `-b`),
  because `pg_upgrade` restores the extension before its table.
  `REPACK (CONCURRENTLY)` is refused whenever the extension exists and the
  table cannot be found, in that mode too.

The design is in [doc/research/fix-design.md](doc/research/fix-design.md).

## Installation

```bash
make
sudo make install        # omit sudo when the PostgreSQL install is yours
```

See [doc/installation.md](doc/installation.md).

## Quick Start

```sql
CREATE EXTENSION pg_repack_safe;
```

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
