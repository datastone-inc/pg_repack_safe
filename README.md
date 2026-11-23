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

> [!IMPORTANT]
> **The warning understates who is affected.** It names only snapshots
> taken before the repack *started*. A snapshot taken while the repack
> runs, before it commits, also reads the table as empty. Section 13.6,
> "Caveats", has it right. This extension covers both cases
> (`t/001_repack_pg_dump.pl` tests a `pg_dump` that starts during the
> repack).

`REPACK (CONCURRENTLY)` is built on the
[pg_squeeze](https://github.com/cybertec-postgresql/pg_squeeze) extension,
not on [pg_repack](https://reorg.github.io/pg_repack/), and both of those
tools have the same caveat: pg_squeeze
documents it, pg_repack does not. Using an extension is a choice, but in
core it was a known trade-off, made for performance and simplicity with a
plan to fix it later. The REPACK (CONCURRENTLY) command's history and its
sources are in [doc/research/history.md](doc/research/history.md).

## Demo incorrect output in action

`demo/demo.sh` runs four small scenarios on a throwaway PostgreSQL 19
server, first without the extension and then with it. It needs only the
PostgreSQL 19 binaries and this extension installed, and it touches no
existing server.

```bash
make install
demo/demo.sh
```

The first scenario is a report that reads `customers`, then `orders`, in one
transaction, while another session repacks `orders` in between. The table
holds 1000 orders worth 10000.00, and the repack changes none of them, so
the right answer is `1000 | 10000.00`. On stock PostgreSQL 19 the report is
told there are no orders:

```text
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS customers FROM customers;
 customers
-----------
       100

-- Meanwhile another session repacks orders.
\! psql -X -q -c "REPACK (CONCURRENTLY) orders"

SELECT count(*) AS orders, sum(amount) AS revenue FROM orders;
 orders | revenue
--------+---------
      0 |
```

With pg_repack_safe, the same statement fails, and the report, run again in
a new transaction, gets the right answer:

```text
SELECT count(*) AS orders, sum(amount) AS revenue FROM orders;
ERROR:  cannot read table "orders" with this snapshot
DETAIL:  REPACK (CONCURRENTLY) rewrote the table after the snapshot was
taken, and row visibility for older snapshots was not preserved.
HINT:  Retry the transaction.
COMMIT;
ROLLBACK

-- The report is retried.
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS customers FROM customers;
 customers
-----------
       100

SELECT count(*) AS orders, sum(amount) AS revenue FROM orders;
 orders | revenue
--------+----------
   1000 | 10000.00

COMMIT;
```

The next two scenarios show an `UPDATE` that reports `UPDATE 0` for ten
matching rows, and a `DELETE` of a customer's orders that removes none of
them before the customer itself is deleted. With the extension both get the
same error.

**The last scenario is `pg_dump`, and it is the one most likely to bite.**
You do not have to choose `REPEATABLE READ` to be exposed: `pg_dump` runs in
a `REPEATABLE READ` transaction of its own. On stock PostgreSQL 19 the dump
succeeds, and the table's rows are missing from it:

```text
pg_dump --data-only --table=orders --snapshot=... > orders.dump
pg_dump exit status: 0
rows in the dump: 0
```

That dump is a backup that cannot restore the table. It is not corrupt in a
way anything detects: the exit status is 0, and the file is well-formed,
with an empty data section where the table's rows belong. Backup jobs and
monitoring report success. The loss shows only when someone restores the
backup and notices (or NOT!) that the restored table is empty, and by then
the source rows may have changed and the older backups that held them may
have been rotated out.

With pg_repack_safe the dump fails instead. A failed backup is noticed and
run again; a silently incomplete one is not:

```text
pg_dump: error: query failed: ERROR:  cannot read table "orders" with this snapshot
DETAIL:  REPACK (CONCURRENTLY) rewrote the table after the snapshot was
taken, and row visibility for older snapshots was not preserved.
HINT:  Retry the transaction.
pg_dump: detail: Query was: COPY public.orders (id, customer_id, amount, status) TO stdout;
pg_dump exit status: 1
rows in the dump: 0

-- The dump is run again, with a snapshot of its own.
pg_dump --data-only --table=orders > orders.dump
pg_dump exit status: 0
rows in the dump: 1000
```

The demo fixes the timing with an exported snapshot (`--snapshot`). Without
one, the exposed dump is one that starts while a `REPACK (CONCURRENTLY)` is
running: `pg_dump` takes its snapshot first and locks each table after, so
it reads a table the repack swapped in the meantime. Once `pg_dump` holds
its lock on a table, a repack of that table cannot swap until the dump
finishes. `t/001_repack_pg_dump.pl` tests both cases with the extension.
On stock PostgreSQL it tests that the incomplete dump restores without an
error, as an empty table.

`t/018_demo.pl` runs the demo and checks every one of these outputs.

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
  [Errors, not wrong rows](#errors-not-wrong-rows).

The reproducers that fail on stock PostgreSQL 19 and pass with the
extension are `demo/demo.sh` (which shows both), and
`specs/repack_concurrently_*.spec`, `t/001_repack_pg_dump.pl`,
`t/008_copy_guard.pl` and `t/013_tablesync.pl`.

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
  ([Requirements](#requirements); `t/012_standby_safe_rewrite.pl`).

Within `REPACK (CONCURRENTLY)` itself, [Limits](#limits) lists the errors
the guard raises where a result might have been correct, and what is lost
when a requirement is not met.

> [!NOTE]
> The guard checks a reader's snapshot against a recorded rewrite, not
> anything specific to `REPACK`, so the same approach may extend to the
> older commands above, in this extension or in a separate one, including
> guarding standby reads after `VACUUM FULL` as it guards them after
> `REPACK (CONCURRENTLY)`. That work has not been designed or tested. If it
> would help you, open an issue and say which commands matter to you.

## Requirements

`make install` only copies the library and the extension's files into the
PostgreSQL installation on the machine where it runs. It changes no
setting and no database, so each item below is done by hand.
[doc/installation.md](doc/installation.md) walks through every step.

- PostgreSQL 19, `wal_level = replica` or higher (required by
  `REPACK (CONCURRENTLY)` itself; `replica` is PostgreSQL's default).
- The same build installed on the primary **and on every physical
  standby**: run `make install`, or install the package, on each host.
- `shared_preload_libraries = 'pg_repack_safe'` on the primary **and on
  every physical standby**, or anything else that replays the WAL, then a
  restart. Add it to any libraries already listed there, such as
  `'pg_stat_statements, pg_repack_safe'`; replacing the list unloads them.
  `REPACK (CONCURRENTLY)` is refused in a session that loaded the library
  any other way. A standby without it fails on the extension's WAL record
  and shuts down ("resource manager with ID 149 not registered").
- `CREATE EXTENSION pg_repack_safe` in every database where
  `REPACK (CONCURRENTLY)` runs; it is refused elsewhere.
- Recommended: `hot_standby_feedback = on`, ideally with a physical
  replication slot, on every hot standby that runs queries. It prevents the
  stock `VACUUM FULL` problem in
  [What pg_repack_safe does not guarantee](#what-pg_repack_safe-does-not-guarantee), and it keeps
  standby snapshots from being ended when the record table is cleaned up
  (see [Standby errors caused by pg_repack_safe](#standby-errors-caused-by-pg_repack_safe-without-hot_standby_feedback)).
  One standby with feedback holds the primary's cleanup back for every
  standby.

[doc/installation.md](doc/installation.md) also covers the order of the
restarts (standbys first), `pg_upgrade`, and other extensions.

### Without the extension

pg_repack_safe needs its library installed and preloaded on every server
that replays the WAL: the primary, its standbys, and any server that
restores a base backup with WAL (point-in-time recovery, clones), for as
long as that WAL holds a `REPACK (CONCURRENTLY)`. That is possible wherever
you manage PostgreSQL yourself. On a managed service it depends on the
provider offering the extension.

Where it cannot be installed, `REPACK (CONCURRENTLY)` has no guard, and any
role with the `MAINTAIN` privilege on a table can run it. To switch it off
for the whole server, set

```ini
max_repack_replication_slots = 0
```

and restart. PostgreSQL then refuses every `REPACK (CONCURRENTLY)` with
`REPACK can only be used if "max_repack_replication_slots" > 0`, while
`REPACK` without `CONCURRENTLY`, which is MVCC-safe on the primary, still
runs
(`t/009_not_preloaded.pl`). The setting can only be changed at server
start, so on a managed service it also depends on the provider exposing it.

### On a managed service

A managed PostgreSQL 19 service that offers neither this extension nor
`max_repack_replication_slots` leaves its customers no setting that stops
`REPACK (CONCURRENTLY)` from returning wrong results. Any role that may run
it on a table (the table's owner, a role granted `MAINTAIN` on it, or a
member of `pg_maintain`) can then make other sessions read that table as
empty and make `pg_dump` write backups without its rows, with no error
anywhere. Customers can only limit who holds those privileges, and a
table's owner can always grant them back to itself.

> [!IMPORTANT]
> If your provider exposes neither, ask them to. Offering one of the two is
> the provider's choice; until they do, the possibility of silently wrong
> results is theirs to remove, not yours (IMO).

## How it works

Each `REPACK (CONCURRENTLY)` of a table with data writes a record, (table,
rewriting transaction ID), into the extension's table
`pg_repack_safe_rewrites`, plus one for the table's TOAST table, and a WAL
record of its own. Once a query holds its locks, it checks the records of
the tables it reads against its snapshot; each backend keeps a copy of the
records, refreshed by cache invalidation, so the check costs about as much
as a hash lookup. The research notes behind the design, with the evidence
for each claim and the log of how it was built, are in
[doc/research/fix-design.md](doc/research/fix-design.md).

Measured on PostgreSQL 19.0 built with optimisation and without assertions,
with select-only `pgbench` (prepared statements, scale 10, three 10-second
runs each, 14-core arm64 machine):

| | 1 client | 8 clients |
| --- | --- | --- |
| without the library | 17,610 tps | 61,200 tps |
| library and extension, no records | 17,470 tps | 61,330 tps |
| a record for the table being read | 17,100 tps | 61,000 tps |

That is at most about 3%, or under 2 microseconds a query, with one client,
where the baseline's own runs differed by 7%; with eight clients the
difference is within run-to-run noise. It is one machine and the workload
where a fixed per-query cost shows most. Each `REPACK (CONCURRENTLY)` also
makes every backend reload its copy of the records once, which is a scan of
a table that holds a handful of rows.

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

A publication `FOR ALL TABLES` includes `pg_repack_safe_rewrites`. The
records a `REPACK (CONCURRENTLY)` writes are not sent to subscribers, so
replication carries on. Exclude the table all the same: a new
subscription's initial copy reads the records already there, a subscriber
that has the extension refuses them, and that one table's copy is retried
until the table is excluded and the subscription refreshed
(`t/016_publication.pl`; see "Logical Replication" in
[doc/installation.md](doc/installation.md)).

The record table is protected against accidents. Its owner and superusers
can still defeat the protection deliberately, for example by replacing one
of its trigger functions:

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
  indexes (a direct `heap_insert()` with `HEAP_INSERT_NO_LOGICAL`, which
  also fires no trigger and is not logically decoded), so an index would
  miss every later record and a query using it would return stale results.
- Adding, dropping or altering a column of it (the type, default, name or
  any other property) is always refused. With any other column list, every
  query in that database would be refused.
- If the record table loses its definition or either of its two triggers by
  a route no hook could stop, every query and `REPACK (CONCURRENTLY)` in
  that database is refused until the extension is re-created.

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

Then check, in each database and on each server, that the guard is in
force:

```sql
SELECT pg_repack_safe_status();
```

It must return `t`. If it returns `f`, with a `WARNING` that says why, the
server did not load the library through `shared_preload_libraries`: the
extension is installed but guards nothing. The extension's description in
`\dx` names this check: "In force only where SELECT
pg_repack_safe_status() returns true".

See [doc/installation.md](doc/installation.md).

## Outlook

Raising an error for older snapshots was proposed on pgsql-hackers while
`REPACK (CONCURRENTLY)` was being developed, and was not adopted: one
session's `REPACK` would make other sessions fail. This extension is that
trade, offered to those who prefer an error to a wrong result. In this
author's opinion, this should have been the default behaviour in PostgreSQL
19 (and before, for TRUNCATE, etc. saying some feature is "not MVCC-safe"
is saying "this operation has incorrect results (and should not have shipped)").

A patch that makes `REPACK (CONCURRENTLY)` MVCC-safe, so that nobody gets an
error or a wrong result, is under review for PostgreSQL 20. It is not
committed, and patches under review are sometimes postponed or dropped, so
do not plan on it. PostgreSQL 19 will not get it either way.

That fix cannot be delivered as an extension. It changes how the heap, the
WAL and logical decoding handle transaction IDs, and PostgreSQL 19 has no
extension point for any of them, which is why `pg_repack_safe` raises an
error instead of returning the right rows. The comparison is in §6 of
[doc/research/history.md](doc/research/history.md), which also gives the
history with its sources.

## Limits

What `REPACK (CONCURRENTLY)` with this extension still costs: errors where
a result might have been correct, and what is lost when a requirement is not
met. Commands other than `REPACK (CONCURRENTLY)` are out of scope (see
[What pg_repack_safe does not guarantee](#what-pg_repack_safe-does-not-guarantee)). Within that
scope, no case of wrong rows is known while the requirements are met.

### Errors, not wrong rows

The guard raises an error when a read *could* return wrong rows, even when
it cannot tell whether this one would. Each case below is an error a retry
in a new transaction gets past, and each says why the guard cannot tell.

- **A table whose only rows were inserted after the snapshot**, and that is
  then repacked: stock PostgreSQL returns a correct empty result. The
  record says only that the table was rewritten after the snapshot, not
  whether any of its rows were visible to it, and the old storage that
  could answer is gone after the swap. A table whose rows were all deleted
  after the snapshot looks the same and does give a wrong result on stock
  PostgreSQL, so it cannot be exempted (`specs/reader_guard_edges.spec`).
- **Foreign-key checks against a repacked table** fail with a foreign-key
  violation (SQLSTATE 23503), not a serialization failure (40001), so a
  client that retries only on 40001 will not retry. This is stock
  PostgreSQL's own error, and the guard does not see the read: PostgreSQL 19
  checks the referenced table's unique index directly, outside the executor
  (`ri_triggers.c`, `ri_FastPathCheck`), for an INSERT or UPDATE in a
  transaction whose snapshot predates a `REPACK (CONCURRENTLY)` of the
  referenced table. The check can only miss a row, never find one wrongly:
  every row of the repacked table carries the REPACK's transaction ID, which
  that snapshot does not see. So the error's message, that the key is not
  present, is false, but no wrong rows are returned or written
  (`specs/reader_guard_edges.spec`).

### Standby errors caused by pg_repack_safe (without `hot_standby_feedback`)

On a hot standby with `hot_standby_feedback = off`, long transactions are
ended some time after a `REPACK (CONCURRENTLY)` on the primary. That
includes transactions that never touch the repacked table. No rows are read
wrongly: the transactions fail instead.

**What you see.** These are PostgreSQL's own recovery-conflict errors,
raised on the standby. None of them mentions pg_repack_safe:

- A running statement is cancelled (SQLSTATE 40001):

  ```text
  ERROR:  canceling statement due to conflict with recovery
  DETAIL:  User query might have needed to see row versions that must be removed.
  ```

- A session that is idle in a transaction, or inside a subtransaction (a
  `SAVEPOINT`, or a PL/pgSQL block with an `EXCEPTION` clause), is
  disconnected instead (`postgres.c`, `report_recovery_conflict`):

  ```text
  FATAL:  terminating connection due to conflict with recovery
  ```

Each one is counted in the standby's `pg_stat_database_conflicts`, in
`confl_snapshot`. An ordinary `VACUUM` causes exactly the same errors on such
a standby, so the errors alone do not show which one caused them.

**How to tell it was the record table.** Set `log_recovery_conflict_waits =
on` on the standby (it is off by default, and takes a reload). The standby
then logs every conflict wait that lasts longer than `deadlock_timeout`
(1 second by default). Each one is logged once while replay waits, before
the sessions are ended, and once when the wait is over. The CONTEXT line
names the WAL record being replayed and its relation, as tablespace,
database and filenode:

```text
LOG:  recovery still waiting after 131.036 ms: recovery conflict on snapshot
DETAIL:  Conflicting processes: 2475040, 2475043.
CONTEXT:  WAL redo at 0/03027AB0 for Heap2/PRUNE_VACUUM_SCAN: snapshotConflictHorizon: 668, ... blkref #0: rel 1663/5/16432, blk 0; ...
```

The DETAIL line lists the processes about to be ended. To name the relation,
connect to the database whose OID is the middle number and pass the first
and last numbers to `pg_filenode_relation`:

```sql
SELECT datname FROM pg_database WHERE oid = 5;
SELECT pg_filenode_relation(1663, 16432);
```

If the second query returns `pg_repack_safe_rewrites`, the conflict came
from the cleanup of the extension's records. Run the lookup soon after the
conflict, while the filenode still belongs to the record table: a rewrite of
the table gives it a new filenode. On the primary, setting the
`log_autovacuum_min_duration` storage parameter of the record table to 0
logs every autovacuum of that table, which shows when the cleanup ran.

**Why.** The record of a `REPACK (CONCURRENTLY)` is what tells a reader that
its snapshot predates the rewrite. Each `REPACK (CONCURRENTLY)` deletes the
records of earlier ones, and a later `VACUUM` of the record table removes
them. When the standby replays that removal, it must first end every
snapshot that could still need a removed record (`standby.c`,
`ResolveRecoveryConflictWithSnapshot`). Otherwise that snapshot would read
the repacked table as empty.

**Who is affected.** Every transaction in the same database on the standby
whose snapshot is older than the latest `REPACK (CONCURRENTLY)` there when
the cleanup is replayed. The standby decides by database and snapshot age.
It cannot know which tables a transaction will read. Other databases on the
standby are not affected.

- Transactions that read the repacked table lose nothing: the guard would
  have refused them anyway.
- The cost falls on everything else: reports, analytics, and `pg_dump` run
  against the standby.

On a database with a steady stream of updates and deletes, ordinary vacuums
already end long standby transactions this way, and the extension adds
little. On a mostly read-only or insert-only database, where such conflicts
are otherwise rare, the extension makes them happen.

**When.** Not at the time of the `REPACK`, but when the record table is
vacuumed:

1. **Autovacuum** vacuums the record table once enough records have been
   deleted (about 50 with its default settings). A manual `VACUUM` of the
   record table vacuums it at once.
2. **The standby** replays the vacuum. It then waits up to
   `max_standby_streaming_delay` for the old snapshots to end before it ends
   them. The default is 30 seconds. When replaying from the WAL archive, the
   setting is `max_standby_archive_delay`.

So the errors can come hours after the `REPACK` that caused them.

With a delay of `-1`, the standby waits as long as it takes instead of
ending anything (`standby.c`, `GetStandbyLimitTime`). Replay stops for the
whole standby, every database included, until the old transactions finish,
and the standby falls behind the primary for that long.

**What to do.** Pick one or more:

- **Turn on `hot_standby_feedback`** on every standby that runs long
  transactions, ideally with a physical replication slot. While the standby
  is connected, the primary keeps the records, along with any other dead
  rows the standby's snapshots need, and nothing is cancelled. The cost is
  bloat on the primary while a long standby transaction runs.
- **Retry.** SQLSTATE 40001 is a serialization failure, and running the
  transaction again from the start is safe. After the `FATAL` error, the
  client has to reconnect first.
- **Raise `max_standby_streaming_delay`** to give long transactions more
  time. The standby then lags behind the primary while they run.
- **Schedule** `REPACK (CONCURRENTLY)`, and any manual `VACUUM` of the record
  table, outside the hours when the standby runs long jobs.

The cancellation, the disconnection and the logged CONTEXT line are pinned
by `t/007_standby_reader.pl` and `t/017_standby_record_cleanup.pl`. The
subtransaction case, the `-1` behaviour and the autovacuum log are stock
PostgreSQL, cited above or documented by it, and are not tested here.

### If the requirements are not met

The guarantees assume the library is preloaded on the primary and on every
standby ([Requirements](#requirements)). Without it:

- **A standby without the library** is misconfigured and can return wrong
  results: it does not check reads, so an old snapshot there reads a
  repacked table as empty. As a best-effort safeguard, not part of the
  guarantee, each `REPACK (CONCURRENTLY)` writes a WAL record that such a
  standby cannot replay, so it usually stops replaying at the first one
  (`t/010_standby_wal_record.pl`). It misses a standby whose replay starts
  from a checkpoint whose redo point lies between a
  `REPACK (CONCURRENTLY)`'s swap and its commit: that standby never sees the
  record (research question 17 in
  [doc/research/fix-design.md](doc/research/fix-design.md)).
- **A session that never loads the library** has no hooks: its
  `REPACK (CONCURRENTLY)` runs unrecorded and its reads are not checked.
  Setting `shared_preload_libraries` leaves no such session. Nothing raises
  an error when it is missing, but `SELECT pg_repack_safe_status()` returns
  `f`, with a `WARNING`. Such a session can also drop the extension while
  its record table holds records, since the refusal is in the library
  (`t/009_not_preloaded.pl`).

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
deleted ones included.

The steps below are safe to stop and resume at any point. Each one ends
with a check; do not go on until it passes. If a check fails, the step
says what to do. Run every SQL command as a superuser.

1. **Stop running `REPACK (CONCURRENTLY)`.** Pause any job that runs it.
   Then check, on the primary, that none is in progress in any database:

   ```sql
   SELECT pid, datname, relid, command FROM pg_stat_progress_repack;
   ```

   Check: no rows. The view also shows plain `REPACK`, `CLUSTER` and
   `VACUUM FULL`; waiting for those too does no harm. If one slips in later,
   nothing breaks: its record survives step 2, step 3 does not reach empty,
   and step 4 is refused. If that happens, start again from step 1.

2. **List the databases that have the extension.** Steps 3 and 4 are
   done in every one of them; a database that is missed keeps the
   extension, and step 5 then leaves it unprotected without an error. On
   the primary:

   ```bash
   psql -XAt -c "SELECT datname FROM pg_database WHERE datallowconn" |
   while IFS= read -r db; do
       psql -XAt -d "$db" -c "SELECT format('database %I: schema %s'
                                            , current_database()
                                            , extnamespace::regnamespace)
                                AS found
                              FROM pg_extension WHERE extname = 'pg_repack_safe'"
   done
   ```

   It prints one line for each database that has the extension, such as
   `database mydb: schema public`.

   Check: you have the list. The schema is the one that holds the
   extension's table and functions, `public` unless the extension was
   created in or moved to another one. If it is not `public`, start each
   session of steps 3 and 4 in that database with, for a schema `ext`:

   ```sql
   SET search_path = ext, public;
   ```

   Otherwise the commands there fail with `relation
   "pg_repack_safe_rewrites" does not exist`.

3. **Delete the records and vacuum them away.** In each database on the
   list, on the primary:

   ```sql
   SELECT pg_repack_safe_prune_records();
   VACUUM (TRUNCATE) pg_repack_safe_rewrites;
   SELECT pg_relation_size('pg_repack_safe_rewrites') = 0 AS empty;
   ```

   Check: `empty` is `t`. `TRUNCATE` is spelled out so that a
   `vacuum_truncate = off` setting, for the server or the table, cannot
   keep the emptied pages.

   If `empty` is `f`, something still holds back the cleanup: `VACUUM`
   keeps a deleted record for as long as any snapshot could need it. This
   lists what holds it in this database:

   ```sql
   SELECT 'session' AS holder, pid::text AS id, backend_xmin AS xmin
   FROM pg_stat_activity
   WHERE datname = current_database() AND backend_xmin IS NOT NULL
     AND pid <> pg_backend_pid()
   UNION ALL
   SELECT 'prepared transaction', gid, transaction
   FROM pg_prepared_xacts WHERE database = current_database()
   UNION ALL
   SELECT 'replication slot', slot_name::text, xmin
   FROM pg_replication_slots WHERE xmin IS NOT NULL
   UNION ALL
   SELECT 'standby feedback', application_name, backend_xmin
   FROM pg_stat_replication WHERE backend_xmin IS NOT NULL;
   ```

   Wait for those to end (a standby with `hot_standby_feedback` reports
   its own oldest query), or end them, then run the `VACUUM` and the
   check again. A standby without `hot_standby_feedback` needs nothing:
   replaying the cleanup ends the standby snapshots that needed the
   records (see [Standby errors caused by pg_repack_safe](#standby-errors-caused-by-pg_repack_safe-without-hot_standby_feedback)).

4. **Drop the extension.** In each database on the list, on the primary:

   ```sql
   DROP EXTENSION pg_repack_safe;
   SELECT count(*) = 0 AS dropped FROM pg_extension WHERE extname = 'pg_repack_safe';
   ```

   Check: `dropped` is `t` in every database on the list. Do not add
   `CASCADE`. If the drop is refused because the record table is not
   empty, go back to step 3. If it is refused because other objects depend
   on the extension (a view of the record table, say), look at them and
   drop them yourself.

5. **Decide whether to take `pg_repack_safe` out of
   `shared_preload_libraries`.** This step is optional. Only do this step 5
   after step 4 has dropped the extension in every database.

   > **Warning: never take the library out while the extension is still
   > installed, that is, never do step 5 before step 4.** That is not a
   > supported configuration, and PostgreSQL does not stop you. Without the
   > library, none of this extension's code runs, so nothing can object. The
   > database looks protected and is not: the record table and functions are
   > still there, and psql's `\dx` still lists `pg_repack_safe`. But
   >
   > - `REPACK (CONCURRENTLY)` succeeds without an error and writes no
   >   record (`t/009_not_preloaded.pl`);
   > - no query is checked, so old snapshots read the repacked table as
   >   empty, as in stock PostgreSQL.
   >
   > The catalog only shows that the extension is installed, not that it is
   > in force. NOTE HOWEVER that `\dx` does say how to check: the description of
   > `pg_repack_safe` reads "In force only where SELECT pg_repack_safe_status()
   > returns true". Run that query. If it returns `f` (false) instead of `t`
   > (true), or fails with `could not access file`, the server is
   > misconfigured. (After step 4 the function is gone, so `function
   > pg_repack_safe_status() does not exist` is expected there.) To fix it,
   > reinstall the package if the file is missing, put `pg_repack_safe` back
   > in `shared_preload_libraries`, restart, and start this procedure again
   > from step 1. Do not just drop the extension in that state: without the
   > library, `DROP EXTENSION` skips the check that refuses it while records
   > exist (`t/009_not_preloaded.pl`).

   At this point the extension is dropped and the library is still in
   `shared_preload_libraries`. Decide what you want `REPACK (CONCURRENTLY)`
   to do:

   - **Blocked:** leave the library in and skip to step 6. With the
     extension dropped, every `REPACK (CONCURRENTLY)` fails with
     `REPACK (CONCURRENTLY) requires the extension "pg_repack_safe" in this
     database` (`sql/storage_change.sql`). This is the safe choice. Running
     `CREATE EXTENSION` again in a database turns the guard back on there.
   - **Allowed again, unprotected:** take the library out, as below. The
     command then behaves as in stock PostgreSQL 19 and can make other
     sessions read the table as empty. To block it after all, set
     `max_repack_replication_slots = 0`
     (see [Without the extension](#without-the-extension)).

   Take it out on the primary first, then on each standby once it has
   replayed the drops. "Each standby" includes cascaded and delayed ones.
   A logical-replication subscriber is a separate server: if it has the
   extension, it needs this whole procedure of its own.

   A standby that is behind may still have to replay the extension's WAL
   record from a recent `REPACK (CONCURRENTLY)`, which it cannot do without
   the library: its replay would stop with "resource manager with ID 149
   not registered". And until it replays the `DROP EXTENSION`, the standby
   still has the extension, so without the library it would look protected
   and not be, as above. Past the drops, nothing the extension wrote is
   left for it to replay.

   On the primary, after the last `DROP EXTENSION`, note the WAL position:

   ```sql
   SELECT pg_current_wal_lsn();
   ```

   On each standby, wait until this returns `t`, with the position from
   the primary in place of `0/3000148`:

   ```sql
   SELECT pg_last_wal_replay_lsn() >= '0/3000148' AS replayed;
   ```

   On each server, find where the setting comes from:

   ```sql
   SELECT setting, source, sourcefile, sourceline
   FROM pg_settings WHERE name = 'shared_preload_libraries';
   ```

   Take `pg_repack_safe` out of the list there and keep the other libraries
   in it, spelled exactly as they are now. A misspelled library stops the
   server from starting.

   - `sourcefile` is `postgresql.conf` or a file it includes: edit that
     line.

     ```ini
     shared_preload_libraries = 'pg_stat_statements'   # was 'pg_stat_statements, pg_repack_safe'
     ```

   - `sourcefile` is `postgresql.auto.conf`, written by `ALTER SYSTEM`: set
     it again with `ALTER SYSTEM`, naming every library that stays, or reset
     it if `pg_repack_safe` was the only one. Do not edit that file by hand.

     ```sql
     ALTER SYSTEM SET shared_preload_libraries = 'pg_stat_statements';
     -- or, if pg_repack_safe was the only library:
     ALTER SYSTEM RESET shared_preload_libraries;
     ```

   - `source` is `command line`: change the server's start command or
     service file. On a managed service, change it in the provider's
     parameter settings.

   Then restart that server the way it is normally restarted (`pg_ctl
   restart -D "$PGDATA"`, `systemctl restart ...`, or the provider's
   console).

   Check: `SHOW shared_preload_libraries;` no longer lists
   `pg_repack_safe`, and still lists every other library.

6. **Uninstall the package** (`make uninstall` with the same `PG_CONFIG`
   as the install, or your package manager), host by host, only when both
   hold for that host:

   - the library is out of `shared_preload_libraries` there (step 5),
     since a server that preloads a library it cannot find does not
     start;
   - nothing kept will be restored on that host from before step 4: no
     base backup taken before the drops, and no archived WAL written
     before them.

   That WAL holds the extension's WAL records, and those backups still
   contain the extension. A server that restores from them (point-in-time
   recovery, a clone, a disaster-recovery host) needs the library
   installed and preloaded for the restore, like a standby; without it,
   recovery stops with "resource manager with ID 149 not registered".
   Keep the package on every host that may restore them until they expire.

Steps 1 to 4 remove the extension without exposing any snapshot or
breaking any standby, and need no backup to have expired.

A superuser can skip step 3 by setting
`pg_repack_safe.allow_unprotected_drop` in the session that runs the drop:

```sql
SET pg_repack_safe.allow_unprotected_drop = on;
DROP EXTENSION pg_repack_safe;
```

A snapshot taken before a recorded `REPACK (CONCURRENTLY)`, on the primary
or on any standby, then reads the repacked table as empty, so this is for
a server where no such snapshot can exist. Set it with `SET`, never in a
configuration file, so it ends with the session.

## Documentation

- [Installation Guide](doc/installation.md)
- [Development Guide](doc/development.md)

## License

MIT; see [LICENSE](LICENSE).
