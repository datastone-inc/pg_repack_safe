# Limits

What `REPACK (CONCURRENTLY)` with this extension still costs: errors where
a result might have been correct, and what is lost when a requirement is not
met. Commands other than `REPACK (CONCURRENTLY)` are out of scope (see
[What pg_repack_safe does not guarantee](../README.md#what-pg_repack_safe-does-not-guarantee)). Within that
scope, the one known case of a wrong result while the requirements are met
is [Contrib functions the guard does not see](#contrib-functions-the-guard-does-not-see).

## Errors, not wrong rows

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

## Contrib functions the guard does not see

The guard checks the tables a query's plan reads and the table
`COPY ... TO` names. Functions of two contrib modules read a table that is
neither: they take the table, or one of its indexes, as an argument and
read the table themselves with the transaction's snapshot. Called with a
snapshot that does not see a committed `REPACK (CONCURRENTLY)` of that
table, they see no rows in it and return a wrong answer with no error:

- **`pgrowlocks('t')`** reports no row locks, whatever rows are locked
  (`contrib/pgrowlocks/pgrowlocks.c`, `pgrowlocks`).
- **`bt_index_check(index, heapallindexed => true)`** and
  `bt_index_parent_check` with the same option, from `amcheck`, find no
  heap row to look up in the index, so they report success without having
  verified that any row is indexed (`contrib/amcheck/verify_nbtree.c`,
  `bt_check_every_level`).
- **`bt_index_check(index, false, true)`**, that is with `checkunique`, and
  `bt_index_parent_check` with the same option compare only the index
  entries of rows the snapshot sees. They see none, compare nothing, and
  report success for a unique index that holds duplicates
  (`contrib/amcheck/verify_nbtree.c`, `heap_entry_is_visible`).

None of them returns the table's rows, and a call in a new transaction is not
affected. A query of the same table in the same transaction gets the
guard's error. PostgreSQL has no hook on a table scan, so the guard cannot
see these reads; no other function in PostgreSQL 19 or its contrib modules
reads a table this way (`t/019_contrib_readers.pl`; the census is in
`doc/research/fix-design.md`, step 12).

## If the requirements are not met

The guarantees assume the library is preloaded on the primary and on every
standby ([Requirements](installation.md#requirements)). Without it:

- **A standby without the library** is misconfigured and can return wrong
  results: it does not check reads, so an old snapshot there reads a
  repacked table as empty. To stop such a standby, each
  `REPACK (CONCURRENTLY)` writes a WAL record that it cannot replay, so it
  stops replaying at the first one (`t/010_standby_wal_record.pl`). This
  check for the misconfiguration misses a standby whose replay starts
  from a checkpoint whose redo point lies between a
  `REPACK (CONCURRENTLY)`'s swap and its commit: that standby never sees the
  record (research question 17 in
  [doc/research/fix-design.md](research/fix-design.md)).
- **A session that never loads the library** has no hooks: its
  `REPACK (CONCURRENTLY)` runs unrecorded and its reads are not checked.
  Setting `shared_preload_libraries` leaves no such session. Nothing raises
  an error when it is missing, but `SELECT repack_safe.status()` returns
  `f`, with a `WARNING`, and each session that loads the library later
  gets the same `WARNING` when it does. Such a session can also drop the
  extension while its record table holds records, since the refusal is in
  the library (`t/009_not_preloaded.pl`).
- **The library in `session_preload_libraries` instead** looks like a
  working setup and is not. Every client session loads the library, warns
  at connection, and refuses `REPACK (CONCURRENTLY)`. A background worker,
  such as a scheduled job, never loads it, so its `REPACK (CONCURRENTLY)`
  runs with no check and no record while interactive sessions refuse the
  same command. The `WARNING` names the setting and says to move the
  library to `shared_preload_libraries` (`t/009_not_preloaded.pl`).
  `local_preload_libraries` is loaded under the same condition
  (`postinit.c`, `InitPostgres`, `INIT_PG_LOAD_SESSION_LIBS`), so it misses
  background workers the same way.

Covered, for the record: `pg_dump`, `COPY ... TO`, logical replication's
initial table copy (refused, and the sync worker retries;
`t/013_tablesync.pl`), and PL/pgSQL procedures that commit inside a loop
(`t/014_plpgsql_toast.pl`).

## Without the extension

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

## On a managed service

A managed PostgreSQL 19 service that offers neither this extension nor
`max_repack_replication_slots` leaves its customers no setting that stops
`REPACK (CONCURRENTLY)` from returning wrong results. Any role that may run
it on a table (the table's owner, a role granted `MAINTAIN` on it, or a
member of `pg_maintain`) can then make other sessions read that table as
empty and make `pg_dump` write backups without its rows, with no error
anywhere. Customers can only limit who holds those privileges, and a
table's owner can always grant them back to itself.

If your provider exposes neither, ask them to offer one of the two.
