# Standby errors caused by pg_repack_safe

This applies to hot standbys that run without `hot_standby_feedback`.

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

If the second query returns `repack_safe.rewrites`, the conflict came
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
