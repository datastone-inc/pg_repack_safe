# Design

This is the argument that the guard is sound, in the form a reviewer can
check: one claim, the lemmas it rests on, and for each lemma the function
in `src/pg_repack_safe.c` that implements it, the PostgreSQL 19 function it
relies on, and the test that exercises it. The research that led here, with
the rejected designs, is in [research/fix-design.md](research/fix-design.md).

## The claim

With the library in `shared_preload_libraries` on the primary and on every
server that replays its WAL, and the extension created in the database:

> A query, or a `COPY ... TO`, that reads a table with a snapshot that does
> not see a committed `REPACK (CONCURRENTLY)` of that table raises an error
> (SQLSTATE 40001) before it returns a row.

The cases where this does not hold are listed under
[Known gaps](#known-gaps).

## Why stock PostgreSQL returns wrong rows

`REPACK (CONCURRENTLY)` copies every row into new storage under its own
transaction ID, then swaps the storage in. A snapshot that does not see
that transaction as committed sees none of the copied rows, and the old
storage, which held the rows it could see, is gone. The table reads as
empty (`specs/repack_concurrently_snapshot.spec`, `demo/demo.sh`).

## Mechanism

- **Writer.** At the swap, the `REPACK (CONCURRENTLY)` transaction inserts
  a record (table OID, its own full transaction ID) into
  `repack_safe.rewrites`, and one for the table's TOAST table.
- **Reader.** Once a query holds its locks, it looks up the tables it reads
  in a backend-local copy of the records and raises the error if a record's
  transaction is committed and not visible to its snapshot.

## Lemmas

### 1. Every `REPACK (CONCURRENTLY)` that can hide rows writes a record

- `isConcurrentRepack` repeats the option loop of `ExecRepack` (`repack.c`),
  the only place that turns the option on, so every spelling of the
  statement is classified (`sql/storage_change.sql`).
- `lockRecordTableForWriter` refuses the statement before it starts unless
  the library was preloaded and the record table exists with the expected
  definition, and locks the record table to the end of the transaction
  (`sql/reader_check.sql`, `t/009_not_preloaded.pl`).
- `noteRelationAltered` runs from `object_access_hook` when
  `swap_relation_files` (`repack.c`) has updated the table's `pg_class` row,
  in the rewriter's transaction and under its `AccessExclusiveLock`, and
  calls `insertRecord`. Storage that had no blocks, or was created in the
  same transaction, held no row an older snapshot could see and is not
  recorded (`sql/writer.sql`).

### 2. A reader that locks the table after the rewrite commits knows the record

- The rewriter holds `AccessExclusiveLock` from the swap to its commit
  (`repack.c`, `rebuild_relation_finish_concurrent`), so a reader gets its
  lock either before the swap, in which case the rewriter waits for it, or
  after the commit.
- `insertRecord` queues a relcache invalidation of the record table. It is
  sent at commit before the locks are released (`xact.c`,
  `CommitTransaction`), and a backend processes pending invalidations when
  it takes a lock (`lmgr.c`, `LockRelationOid`). The reader's cached copy
  is therefore marked stale before its lock is granted.
- `loadRecordCache` reloads the copy with `SnapshotAny`, so it holds every
  row version, and repeats the load if an invalidation arrived during it
  (`specs/reader_cache.spec`, `t/021_discard_caches.pl`).

### 3. The check runs after the locks and covers every read

- `executorStartHook` calls the check after `standard_ExecutorStart`, which
  takes the plan's relation locks and opens every relation the plan reads
  (`execUtils.c`, `ExecGetRangeTableRelation`). `checkUnsafeRewriteRecords`
  checks each opened relation, and every range-table relation when the plan
  has a `CustomScan` (`specs/unread_relations.spec`).
- `COPY relation TO` reads outside the executor (`copyto.c`, `DoCopyTo`).
  `checkCopyTo` locks the relation and checks it in `ProcessUtility_hook`;
  a partitioned table is run as `COPY (SELECT ...) TO`, which the executor
  check covers (`t/008_copy_guard.pl`, `specs/copy_attach_race.spec`).
- An `INSERT`'s target is written, not read through the snapshot, and is
  not checked, unless a foreign key of the table references the table
  itself (`specs/reader_guard_edges.spec`).

### 4. The verdict for one record is right

`rewriteHiddenFromSnapshot` returns true only if the record's transaction
is in progress or in the future for the snapshot (`snapmgr.c`,
`XidInMVCCSnapshot`) and committed (`TransactionIdDidCommit`). Records
carry 64-bit transaction IDs; one more than 2^31 transactions old precedes
every live snapshot and is passed without comparing its 32-bit form
(`sql/reader_check.sql`, `t/006_record_xid8.pl`). A read without an MVCC
snapshot of a table that has a record is refused.

### 5. A record outlives every snapshot that needs it

- Each `REPACK (CONCURRENTLY)`, and `repack_safe.prune_records()`, deletes
  only records of transactions older than the deleting one, and never a row
  the deleting transaction inserted (`deleteOlderRecords`).
- The deleted row version stays in the table, and in every reader's copy,
  until `VACUUM` removes it, which it does only once the deleting
  transaction precedes every snapshot of the database
  (`heapam_visibility.c`, `HeapTupleSatisfiesVacuumHorizon`). A snapshot
  that does not see the recorded rewrite is older than the deletion
  (`specs/record_prune.spec`).
- On a standby the removal is replayed with a conflict horizon, and replay
  first ends every snapshot that could need the record (`standby.c`,
  `ResolveRecoveryConflictWithSnapshot`; `t/002_standby_record_removal.pl`,
  `t/017_standby_record_cleanup.pl`).
- Everything else that would remove or falsify a record is refused, by the
  record table's two triggers and by `object_access_hook`
  ([Record table protection](#record-table-protection);
  `sql/protection.sql`, `t/022_record_table_rename.pl`).

### 6. Every backend that can read the table has the check

- Only `shared_preload_libraries` loads the library into every backend,
  background workers included. `_PG_init` notes how it was loaded, and
  `REPACK (CONCURRENTLY)` is refused in a backend that loaded it any other
  way (`t/009_not_preloaded.pl`).
- A standby reads the records as replicated heap rows and gets the same
  invalidation from the commit record (`t/007_standby_reader.pl`).
- A server that replays the WAL without the library stops at the custom WAL
  record that `logUnsafeRewrite` writes at the swap, before the commit that
  would make the rewritten table visible there (`rmgr.c`, `RmgrNotFound`;
  `t/010_standby_wal_record.pl`).

## PostgreSQL internals the guard relies on

A change to any of these in a PostgreSQL 19 minor release could break the
guard. CI builds the tip of `REL_19_STABLE` every week and runs all three
suites against it, to learn of such a change before it ships.

| Behaviour | PostgreSQL source | Test that exercises it |
| --- | --- | --- |
| At `OAT_POST_ALTER` of the swap, the catalog cache still holds the old `pg_class` row and a `SnapshotSelf` scan returns the new one | `repack.c`, `swap_relation_files` | `sql/storage_change.sql`, `sql/writer.sql` |
| The executor opens every relation a plan reads while it starts, into `es_relations` | `execUtils.c`, `ExecGetRangeTableRelation` | `specs/unread_relations.spec` |
| Invalidations are sent at commit before locks are released, and processed when a lock is taken | `xact.c`, `CommitTransaction`; `lmgr.c`, `LockRelationOid` | `specs/reader_cache.spec` |
| `REPACK (CONCURRENTLY)` holds `AccessExclusiveLock` from the swap to its commit | `repack.c`, `rebuild_relation_finish_concurrent` | `specs/repack_concurrently_snapshot.spec` |
| A `heap_insert` with `HEAP_INSERT_NO_LOGICAL` is not decoded | `decode.c`, `DecodeInsert` | `t/016_publication.pl` |
| Replaying a row removal ends the standby snapshots older than its conflict horizon | `standby.c`, `ResolveRecoveryConflictWithSnapshot` | `t/017_standby_record_cleanup.pl` |
| Replay fails on a record of an unregistered resource manager | `rmgr.c`, `RmgrNotFound` | `t/010_standby_wal_record.pl` |

## Known gaps

Each is described, with its reproducer, in [limits.md](limits.md).

- Functions of `pgrowlocks` and `amcheck` read a table they take as an
  argument, outside the executor, and return a wrong answer with no error
  (`t/019_contrib_readers.pl`).
- A foreign-key check against a repacked table fails with a foreign-key
  violation (23503), not 40001: an error, with a misleading message
  (`specs/reader_guard_edges.spec`).
- A standby without the library whose replay starts between a swap and its
  commit never sees the custom WAL record and is not stopped.

## Cost

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

## COPY of a partitioned table

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

## Publications

A publication `FOR ALL TABLES` includes `repack_safe.rewrites`. The
records a `REPACK (CONCURRENTLY)` writes are not sent to subscribers, so
replication carries on. Exclude the table all the same: a new
subscription's initial copy reads the records already there, a subscriber
that has the extension refuses them, and that one table's copy is retried
until the table is excluded and the subscription refreshed
(`t/016_publication.pl`; see "Logical Replication" in
[installation.md](installation.md)).

## Record table protection

The record table is protected against accidents. Its owner and superusers
can still defeat the protection deliberately, for example by replacing one
of its trigger functions:

- Each `REPACK (CONCURRENTLY)` adds one or two records and deletes every
  older one, so the table stays small. A deleted record still protects: it
  stays in the table until `VACUUM` finds that no snapshot can need it.
  `repack_safe.prune_records()` deletes the remaining records in a
  database that has stopped repacking.
- While it holds records, `DROP` of the table, its triggers or the
  extension, and `TRUNCATE` of the table, are refused unless a superuser
  sets `pg_repack_safe.allow_unprotected_removal` (see
  [Removing the extension](removal.md)). So are renaming the
  table and moving it to another schema, because the extension finds the
  table by its name in its own schema and would no longer protect it.
  The extension itself cannot be moved: its schema is fixed
  (`t/022_record_table_rename.pl`). `UPDATE` and `DELETE`
  of records, rewrites that do not keep every record unchanged (a rewriting
  `ALTER TABLE`, `REPACK (CONCURRENTLY)`), and changes to when its triggers
  fire are always refused. `VACUUM` of it is always allowed.
- `VACUUM FULL`, `CLUSTER` and `REPACK` of it are refused while it holds a
  deleted record, because they would drop the record without ending the
  standby snapshots that still need it. Run `VACUUM
  repack_safe.rewrites` first and repeat the command. The database-wide
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
