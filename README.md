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

`REPACK` is a command built into PostgreSQL 19, and this extension guards
that command. It is not related to the `pg_repack` extension.

</div>
<!-- markdownlint-enable MD033 MD041 -->

---

## The problem

PostgreSQL 19's `REPACK (CONCURRENTLY)` rewrites a table without blocking
reads or writes. Its reference page carries this warning, under
["Notes on Concurrent Operation"](https://www.postgresql.org/docs/19/sql-repack.html#SQL-REPACK-NOTES-ON-CONCURRENTLY):

> [!WARNING]
> `REPACK` with the `CONCURRENTLY` option is not MVCC-safe. After the
> repacking transaction commits, the table will appear empty to concurrent
> transactions, if they are using a snapshot taken before repacking started.
> See [Section 13.6](https://www.postgresql.org/docs/19/mvcc-caveats.html)
> for more details.

In practice, after it commits, a transaction whose snapshot was taken
before it committed, and which had not yet touched the table, reads the
table as **empty**. Nothing raises an error. The same happens to `pg_dump`
(a dump with the table's rows missing), to cursors, and to logical
replication's initial table copy (a subscriber that never gets the table's
existing rows).

The warning understates who is affected: a snapshot taken while the repack
runs, before it commits, also reads the table as empty.
[doc/problem.md](doc/problem.md) has the details, the command's lineage and
a demo that shows four cases on a throwaway server.

## What the extension does

A report reads `customers`, then `orders`, in one transaction, and another
session repacks `orders` in between. On stock PostgreSQL 19 the report is
told there are no orders:

```text
SELECT count(*) AS orders, sum(amount) AS revenue FROM orders;
 orders | revenue
--------+---------
      0 |
```

With pg_repack_safe the same statement fails, and the report, run again in
a new transaction, gets the right answer:

```text
SELECT count(*) AS orders, sum(amount) AS revenue FROM orders;
ERROR:  cannot read table "orders" with this snapshot
DETAIL:  REPACK (CONCURRENTLY) rewrote the table after the snapshot was
taken, and row visibility for older snapshots was not preserved.
HINT:  Retry the transaction.
```

`pg_dump` is the case most likely to bite: on stock PostgreSQL 19 a dump
that starts during a `REPACK (CONCURRENTLY)` exits with status 0 and the
table's rows missing. With the extension it fails and is run again.

## Installation

On the primary and on every standby, build and install, add the library to
`shared_preload_libraries`, and restart (standbys first):

```bash
make
sudo make install        # omit sudo when the PostgreSQL install is yours
```

```ini
shared_preload_libraries = 'pg_repack_safe'   # add to any existing list
```

Then, in each database where `REPACK (CONCURRENTLY)` will run:

```sql
CREATE EXTENSION pg_repack_safe;
```

Check, in each database and on each server, that the guard is in force:

```sql
SELECT repack_safe.status();
```

It must return `t`. If it returns `f`, with a `WARNING` that says why, the
server did not load the library through `shared_preload_libraries`: the
extension is installed but guards nothing. The extension's description in
`\dx` names this check.

The primary and its standbys must run the same build of the library. On
each server:

```sql
SELECT module_name, version FROM pg_get_loaded_modules()
WHERE module_name = 'pg_repack_safe';
```

The extension's table and functions are always in the schema
`repack_safe`. [doc/installation.md](doc/installation.md) has the
requirements in full, the settings, `pg_upgrade`, logical replication and
other extensions.

## What it costs

- **Old snapshots get an error.** A transaction whose snapshot predates a
  `REPACK (CONCURRENTLY)` of a table and that then reads the table fails
  with SQLSTATE 40001 and has to be retried. That includes a `pg_dump`
  that started during the repack.
- **Standbys without `hot_standby_feedback` cancel long transactions**
  some time after a `REPACK (CONCURRENTLY)` on the primary, including
  transactions that never read the repacked table
  ([doc/standbys.md](doc/standbys.md)).
- **A server that replays the WAL without the library stops** at the first
  `REPACK (CONCURRENTLY)`, with "resource manager with ID 149 not
  registered": a standby, or a restore of a base backup with WAL. That is
  deliberate, since such a server could not check its reads.
- **A fixed cost per query**, measured at 3% or less on select-only
  `pgbench` with one client and within noise with eight
  ([doc/design.md](doc/design.md#cost)).

## What pg_repack_safe guarantees

With the library in `shared_preload_libraries` on the primary and on every
standby, and the extension created in the database:

- A read of a table, including the read an `UPDATE` or `DELETE` does to
  find its rows, whose snapshot does not see a committed
  `REPACK (CONCURRENTLY)` of that table, fails with
  `ERROR: cannot read table "..." with this snapshot`, SQLSTATE 40001 (a
  serialization failure: retry the transaction), instead of returning wrong
  rows. This covers every query through the executor (views, functions,
  cursors, parallel plans, partitions, the TOAST table), `COPY ... TO`
  (including partitioned tables, `pg_dump`, and logical replication's
  initial copy), on the primary and on hot standbys. An `INSERT` writes its
  target table without reading it, so the target is not checked; a read of
  the same table in the same statement is (`INSERT ... SELECT`, a subquery,
  the query around a data-modifying `WITH`), and so is a target with a
  foreign key to itself, whose check reads it. `ON CONFLICT` on a row the
  snapshot cannot see fails with stock PostgreSQL's own serialization
  failure (`specs/reader_guard_edges.spec`).
- Reads that do not need an error get none: snapshots taken after the
  repack committed, snapshots that had already read the table (the repack
  waits for them), empty tables, `VACUUM FULL`, `CLUSTER`, plain `REPACK`.
  The exceptions, where the guard errors and stock PostgreSQL would have
  been right, are listed under
  [Errors, not wrong rows](doc/limits.md#errors-not-wrong-rows).

The reproducers that fail on stock PostgreSQL 19 and pass with the
extension are `demo/demo.sh` (which shows both), and
`specs/repack_concurrently_*.spec`, `t/001_repack_pg_dump.pl`,
`t/008_copy_guard.pl` and `t/013_tablesync.pl`.

How the guard works, and the argument that it is sound, are in
[doc/design.md](doc/design.md).

## What pg_repack_safe does not guarantee

The extension guards `REPACK (CONCURRENTLY)` and nothing else. Other
commands keep their stock behaviour, including those that rewrite a table
without preserving visibility. All of them predate PostgreSQL 19, and a
test pins the stock behaviour of each with the extension loaded:

- **`TRUNCATE` and rewriting `ALTER TABLE`** (for example a column type
  change): a snapshot taken before them reads the table as empty. Upstream
  documents both as not MVCC-safe (`specs/stock_rewrites.spec`).
- **`REFRESH MATERIALIZED VIEW`** without `CONCURRENTLY`: a snapshot taken
  before it reads the view's *new* contents, because the refresh writes its
  rows frozen. Upstream does not document this. `REFRESH ... CONCURRENTLY`
  is not affected (`specs/stock_rewrites.spec`).
- **`VACUUM FULL`, `CLUSTER` and plain `REPACK` with hot standbys that run
  without `hot_standby_feedback`**: the primary can drop rows that a
  standby snapshot still needs, and the standby query silently misses
  them. Reported on pgsql-hackers in 2012 and still unfixed. Turning on
  `hot_standby_feedback` prevents it
  ([Requirements](doc/installation.md#requirements); `t/012_standby_safe_rewrite.pl`).

Within `REPACK (CONCURRENTLY)` itself, [Limits](doc/limits.md) lists the errors
the guard raises where a result might have been correct, and what is lost
when a requirement is not met.

> [!NOTE]
> The guard checks a reader's snapshot against a recorded rewrite, not
> anything specific to `REPACK`, so the same approach may extend to the
> older commands above, in this extension or in a separate one, including
> guarding standby reads after `VACUUM FULL` as it guards them after
> `REPACK (CONCURRENTLY)`. That work has not been designed or tested. If it
> would help you, open an issue and say which commands matter to you.

Three gaps are known within `REPACK (CONCURRENTLY)` itself, each with a
reproducer in [doc/limits.md](doc/limits.md):

- Functions of the contrib modules `pgrowlocks` and `amcheck` read a table
  outside the executor. With an old snapshot they return a wrong answer and
  no error.
- A foreign-key check against a repacked table, from an old snapshot, fails
  with a foreign-key violation (23503) where the other reads fail with
  40001.
- A standby without the library whose replay starts between a repack's swap
  and its commit is not stopped.

## Turning it off

In a hurry, as a superuser, in each database:

```sql
SET pg_repack_safe.allow_unprotected_removal = on;
DROP EXTENSION pg_repack_safe;
```

No read is checked in that database from then on, and a snapshot taken
before a recorded `REPACK (CONCURRENTLY)` reads the repacked table as
empty. With the library still preloaded, `REPACK (CONCURRENTLY)` is refused
in a database without the extension, so no new case arises. No restart is
needed. [doc/removal.md](doc/removal.md) has the procedure that exposes no
snapshot, and what to do before taking the library out of
`shared_preload_libraries`.

To switch `REPACK (CONCURRENTLY)` off for a whole server, with or without
the extension, set `max_repack_replication_slots = 0` and restart
([doc/limits.md](doc/limits.md#without-the-extension)).

## Outlook

Raising an error for older snapshots was proposed on pgsql-hackers while
`REPACK (CONCURRENTLY)` was being developed, and was not adopted: one
session's `REPACK` would make other sessions fail. This extension is that
trade, offered to those who prefer an error to a wrong result.

A patch that makes `REPACK (CONCURRENTLY)` MVCC-safe, so that nobody gets an
error or a wrong result, is under review for PostgreSQL 20. It is not
committed, and PostgreSQL 19 will not get it either way. That fix cannot be
delivered as an extension: it changes how the heap, the WAL and logical
decoding handle transaction IDs. The history, with its sources, and the
comparison are in [doc/research/history.md](doc/research/history.md).

## Documentation

- [The problem in detail, and the demo](doc/problem.md)
- [Installation, settings, upgrades, logical replication](doc/installation.md)
- [Design: why the guard is sound, and what it costs](doc/design.md)
- [Limits: errors, known gaps, unmet requirements](doc/limits.md)
- [Standby errors without `hot_standby_feedback`](doc/standbys.md)
- [Removing the extension](doc/removal.md)
- [Development](doc/development.md)

## License

MIT; see [LICENSE](LICENSE).
