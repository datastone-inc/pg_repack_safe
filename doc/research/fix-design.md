# Preventing wrong results from non-MVCC-safe rewrites: design research

Research notes, 2026-11-15, against PostgreSQL REL_19_STABLE at db9095b9026
(19rc2 plus 42 commits; five commits short of 19.0, none in the code cited).
Tags: **[V]** read in the code, **[I]** inferred and needs a reproducer,
**[R]** confirmed by this repo's tests. Source references name the file and,
in parentheses, the function. **Status: direction chosen 2026-11-15
(record-based reader guard, see "Design direction").**

## Bottom line

1. Old snapshots see an empty table because of **xmin stamping**, not because
   the old file is gone. REPACK CONCURRENTLY and rewriting ALTER TABLE insert
   every row into a new relfilenode with the rewriter's own XID; TRUNCATE
   installs an empty relfilenode.
2. The dangerous snapshots are those taken **before the rewrite commits**, not
   "before repacking started". [R] `t/001_repack_pg_dump.pl` case 2.
3. **A writer-side check before commit cannot be both race-free and usable.**
   At commit every other snapshot in the database is a potential victim, and a
   new one can appear between any check the extension can run and the commit.
   A wait *after* commit under a session lock is race-free on the primary
   (candidate H) but locks the table until old snapshots end.
4. **Chosen: a record-based reader-side guard (candidate G).** The writer
   records each unsafe rewrite; once a reader holds its lock on a relation, it
   errors (40001) if the relation has a record whose XID its snapshot does not
   see as committed. This is the `relcheckxmin` idea rejected for core in
   2026-02 (see [history.md](history.md) §3), with an extension-owned record
   in place of a catalog column.
5. **Refusing REPACK as the only defence, with no reader-side guard, is
   rejected.** A refusal has to be decided before commit, and no check before
   commit can be race-free ([§3](#3-timing)): a snapshot taken after the check
   still sees an empty table, and nothing else catches it. And because any
   snapshot at all counts as old ([§2](#2-writer-side-detecting-an-older-snapshot-exists)),
   a check strict enough to be correct would refuse almost every time on a
   busy database.

## 1. Mechanism

**REPACK CONCURRENTLY** [V]

- Takes ShareUpdateExclusiveLock (`repack.c (RepackLockLevel)`), starts the decoding
  worker, copies with the worker's exported historic snapshot
  (`repack.c (rebuild_relation)`, `get_initial_snapshot`;
  `repack_worker.c (RepackWorkerMain)`).
- Copy: `heap_insert_for_repack` → `heap_insert(NewHeap, ...,
  GetCurrentCommandId(true), HEAP_INSERT_NO_LOGICAL)`
  (`heapam_handler.c (heap_insert_for_repack)`, called from
  `heapam_relation_copy_for_cluster`). No rewriteheap: xmin
  is not preserved, nothing frozen, every row's xmin is the REPACK XID.
- MVCC scan with the initial snapshot (`heapam_handler.c (heapam_relation_copy_for_cluster)`);
  recently-dead rows are dropped.
- Concurrent changes replayed in the same transaction (`repack.c (apply_concurrent_insert)`).
- Lock upgrade `LockRelationOid(..., AccessExclusiveLock)` (`repack.c (rebuild_relation_finish_concurrent)`),
  indexes/TOAST, final replay, index swaps and `finish_heap_swap` in the same
  function → `swap_relation_files` updates pg_class relfilenode, so
  t's pg_class xmin becomes the REPACK XID.
- The backend gets its XID after the initial snapshot (comment
  `repack.c (ExecRepack)`).
- A reader acquiring its lock runs `AcceptInvalidationMessages` (`lmgr.c (LockRelationOid)`)
  and moves to the new relfilenode; old files are unlinked after locks are
  released (`xact.c (CommitTransaction)`: `smgrDoPendingDeletes` follows
  the lock release).

**TRUNCATE** [V]: AccessExclusiveLock up front (`tablecmds.c (ExecuteTruncate)`), then
`RelationSetNewRelfilenumber` (`tablecmds.c (ExecuteTruncateGuts)`; `relcache.c (RelationSetNewRelfilenumber)`). The
in-place path (`tablecmds.c (ExecuteTruncateGuts)`) applies only to a relation created or
re-filed in the current subtransaction, which no other snapshot can see.

**ALTER TABLE rewrite** [V]: `ATRewriteTables` → `make_new_heap`
(`tablecmds.c (ATRewriteTables)`) → `ATRewriteTable`, scanning with `GetLatestSnapshot()`
and inserting with the current command ID, then `finish_heap_swap` in
`ATRewriteTables`. Same effect as REPACK CONCURRENTLY.

**REFRESH MATERIALIZED VIEW** (non-concurrent, not in mvcc-caveats): fills with
`TABLE_INSERT_FROZEN` (`matview.c (transientrel_startup)`) and swaps (`refresh_by_heap_swap`). [I] An old snapshot
sees the *new* contents, not an empty view.

**REPACK (plain), CLUSTER, VACUUM FULL: MVCC-safe on the primary** [V]. All go
through `cluster_rel` (`vacuum.c (vacuum_rel)`; CLUSTER is a `RepackStmt`). SnapshotAny
scan keeping RECENTLY_DEAD rows (`heapam_handler.c (heapam_relation_copy_for_cluster)`);
`rewrite_heap_tuple` copies xmin/xmax verbatim (`rewriteheap.c (rewrite_heap_tuple)`),
freezing only below FreezeLimit ≤ OldestXmin (`vacuum.c (vacuum_get_cutoffs)`;
`procarray.c (ComputeXidHorizons)`). [R] Not on a standby without
hot_standby_feedback: its query may need rows VACUUM FULL dropped as DEAD,
and no conflict cancels it, so it silently misses them (research question
1, `t/012_standby_safe_rewrite.pl`; a known gap in stock PostgreSQL).

**Other wrong-results modes in REPACK CONCURRENTLY**

- Logical replication initial sync: tablesync uses a USE_SNAPSHOT slot snapshot
  (`walsender.c (CreateReplicationSlot)`) then COPYs; if REPACK commits in between, the
  subscriber's initial copy is empty and the rows are lost for good. [R]
  `t/013_tablesync.pl`, which runs the sync worker's commands on a replication
  connection rather than through a subscription; it fails on stock PG19.
- Hot standby: standby snapshots taken before commit replay see the table
  empty; the primary cannot see them. [R] with the guard bypassed (step 5's
  reverted cleanup design; the `t/017` mutation run); no stock-only test.
  Guarded: `t/007_standby_reader.pl`, `t/017_standby_record_cleanup.pl`.
- Swap-time lock upgrade (`repack.c (rebuild_relation_finish_concurrent)`): readers
  queue behind it and are victims if their snapshot predates the commit. [R]
- Serializable: predicate locks promoted at swap (`repack.c
  (rebuild_relation_finish_concurrent)`); no extra hazard seen. [V]
- TOAST re-toasted with new value OIDs (`repack.c (copy_table_data)`); open risk is a
  detoast-later pointer held across a procedure COMMIT. [I, low]
- Rejected targets: catalogs, user_catalog_table, TOAST, unlogged/temp,
  matviews, partitioned (`repack.c (check_concurrent_repack_requirements)`, `repack.c (ExecRepack)`). [V]

## 2. Writer side: detecting "an older snapshot exists"

- Core primitive: `GetCurrentVirtualXIDs` (`procarray.c (GetCurrentVirtualXIDs)`), as used by
  `WaitForOlderSnapshots` (`indexcmds.c (WaitForOlderSnapshots)`), excluding
  `PROC_IS_AUTOVACUUM | PROC_IN_VACUUM | PROC_IN_SAFE_IC`, waiting with
  `VirtualXactLock`. [V]
- **xmin filtering excludes nothing**: every snapshot existing before our
  commit has xmin ≤ our XID. The real test is "any other backend in this
  database holds any snapshot", which on a busy system is always true.
- `allDbs = false` is right (other databases cannot read the table).
- Slots: an EXPORT_SNAPSHOT/USE_SNAPSHOT walsender appears with a PGPROC xmin
  and `PROC_IN_LOGICAL_DECODING` (`logical.c (StartupDecodingContext)`); do not exclude that
  flag, except REPACK's own decoding worker (same lock group).
- hot_standby_feedback gives only an aggregate xmin; standbys without it are
  invisible. **The writer side cannot cover standbys.**
- Prepared transactions hold no snapshot (`twophase.c (MarkAsPreparingGuts)`); not victims.
- WITH HOLD cursors are materialized in `PreCommit_Portals` (`xact.c (CommitTransaction)`)
  while locks are held: safe. Plain cursors calling functions that open other
  relations later are victims. [R] `specs/repack_concurrently_cursor.spec`.
- Nothing records whether a snapshot will touch t; PGPROC does not even record
  isolation level.

## 3. Timing

- Dangerous set: snapshots taken before the rewriter's `ProcArrayEndTransaction`
  (`xact.c (CommitTransaction)`) that later read t under a lock acquired after the rewriter
  released AccessExclusiveLock. [V, R]
- A check at ProcessUtility start misses snapshots taken during the copy and
  at the swap. [R]
- No pre-commit check is race-free: snapshots taken between the check and
  commit are dangerous, and an extension cannot hold ProcArrayLock across
  commit. Latest usable points: `XACT_EVENT_PRE_COMMIT` (`xact.c
  (CommitTransaction)`) and `OAT_POST_ALTER` in `swap_relation_files` (`repack.c
  (swap_relation_files)`), both with a window, the latter after all the work.
- `XACT_EVENT_COMMIT` (`xact.c (CommitTransaction)`) is after commit visibility
  but before lock release; waiting there and cancelling queued victims would
  close the window on the primary, but it runs under `HOLD_INTERRUPTS`, cannot
  ERROR, and cannot use the deadlock detector. Not recommended.
- A CIC-style wait before commit is correct when it ends in a deadlock error
  but can livelock as new snapshots keep appearing. [I] (CIC is `CREATE INDEX
  CONCURRENTLY`, which before finishing waits for every transaction whose
  snapshot predates its own: `WaitForOlderSnapshots`, §2.)

## 4. Hook points

PG19 `*_hook_type`s: ProcessUtility, ExecutorStart/Run/Finish/End,
ExecutorCheckPerms, post_parse_analyze, planner (+ setup/shutdown), path and
join hooks, stats hooks, object_access (+ `_str`), fmgr, row_security_policy,
emit_log, auth hooks, shmem_request/startup, EXPLAIN hooks. Callbacks:
`RegisterXactCallback`, `RegisterSubXactCallback`,
`CacheRegisterRelcacheCallback`. No REPACK-specific hook; only the test-only
`INJECTION_POINT("repack-concurrently-before-lock")` (`repack.c (rebuild_relation_finish_concurrent)`).

Writers by parse node: `T_RepackStmt` (`utility.c (standard_ProcessUtility)`; REPACK, CLUSTER;
CONCURRENTLY only for `REPACK_COMMAND_REPACK`, `repack.c (ExecRepack)`),
`T_VacuumStmt` FULL, `T_TruncateStmt`, `T_AlterTableStmt`,
`T_RefreshMatViewStmt`.

ProcessUtility cannot see: TRUNCATE CASCADE targets (resolved in
`ExecuteTruncateGuts`), logical-replication apply TRUNCATE (`worker.c (apply_handle_truncate)`
calls `ExecuteTruncateGuts` directly), and which ALTER subcommands rewrite.
Lower-level catch points: `OAT_TRUNCATE` (`tablecmds.c (truncate_check_rel)`), `OAT_POST_ALTER`
at every `swap_relation_files`, the `table_rewrite` event trigger (ALTER only).

## 5. TRUNCATE and rewriting ALTER

Same dangerous set. Taking AccessExclusiveLock immediately only protects
readers that already touched t. The record-based guard (§6b) covers them only
if their writers also write a record. Proposed decision 4 leaves them
unrecorded for now. What recording them would take:

- Heap storage is replaced in exactly five places [V]: the two `cluster_rel`
  swaps (`repack.c (rebuild_relation)` non-concurrent, `repack.c (rebuild_relation_finish_concurrent)` concurrent), ALTER
  rewrite (`tablecmds.c (ATRewriteTables)`), REFRESH MATERIALIZED VIEW (`matview.c (refresh_by_heap_swap)`),
  and TRUNCATE (`RelationSetNewRelfilenumber`, `tablecmds.c (ExecuteTruncateGuts)`). SET
  TABLESPACE copies blocks and is MVCC-safe.
- TRUNCATE: `OAT_TRUNCATE` fires in `truncate_check_rel` for named and
  CASCADE targets (`tablecmds.c (ExecuteTruncate)`, `tablecmds.c (ExecuteTruncateGuts)`) but not for logical-replication
  apply, which never calls `truncate_check_rel` [V: no call in `worker.c`].
  `RelationSetNewRelfilenumber` has no hook of its own.
- ALTER rewrite: no dedicated C hook. `OAT_POST_ALTER` fires from 35 pg_class
  call sites and does not say "this was a swap"; the `table_rewrite` event
  trigger (`tablecmds.c (ATRewriteTables)`) is exact but can be disabled, which fails open.
- A general detector ("relfilenode differs at pre-commit from what it was at
  the first hook call, and the command is not a known MVCC-safe one") would
  cover all of them including logical-replication TRUNCATE. [I]

## 6. Reader side

- No relation-open hook. Usable: `ExecutorStart_hook` (call through first so
  locks are held and the range table is final) and `ProcessUtility_hook` for
  COPY TO (`copyto.c (CopyRelationTo)` scans with `GetActiveSnapshot()`; lock first, check,
  call through).
- Precedents: `indcheckxmin` (`plancat.c (get_relation_info)`) compares pg_index xmin with the
  snapshot; the 2012 "Making TRUNCATE more MVCC-safe" RFC (relvalidxmin);
  "snapshot too old" (removed in f691f5b80a8, PG17; page-LSN based, not a
  precedent for this).
- Race-freedom (both variants below): every unsafe rewrite holds
  AccessExclusiveLock until commit, and the reader holds at least
  AccessShareLock from the check to the end of its transaction, so no rewrite
  can commit between check and read (`plancache.c (AcquireExecutorLocks)`,
  `execUtils.c (ExecGetRangeTableRelation)`). [V]
- Gaps common to both variants:
  - the reader fails, not the rewrite;
  - needs `shared_preload_libraries` on the primary and every standby;
  - misses readers that bypass the executor and COPY (direct
    `table_beginscan` in other extensions, output plugins reading
    user_catalog_tables).

### 6a. Rejected variant: pg_class xmin (candidate E)

Per RTE_RELATION, compare the xmin of t's current pg_class row with S; if S
does not see it, compare relfilenode as of S and raise 40001 on a difference.
Rejected 2026-11-15 for two reasons:

- **Spurious errors, which the developer ruled unacceptable.** It errors where
  stock PG returns correct rows: after MVCC-safe rewrites (VACUUM FULL,
  CLUSTER, plain REPACK, SET TABLESPACE) and for tables created after S.
- **Fail-open on the reader's own DDL.** [I] S taken → REPACK commits → the
  same session runs GRANT or `ALTER TABLE t SET (...)` (catalog updates use a
  current snapshot, so they succeed) → pg_class xmin is the reader's own XID →
  the "own transaction" rule passes → t reads empty. A lookup as of S does not
  help, because S sees the session's own new row by command ID. pg_class xmin
  records who last wrote the catalog row, not who installed the storage.

### 6b. Chosen variant: unsafe-rewrite record (candidate G)

Nothing here is prototyped at the time of design; every step is [I] unless tagged.

**Writer.** When REPACK (CONCURRENTLY) swaps a table, insert a row
(table OID, rewriter XID) into an extension-owned table, in the rewriter's own
transaction. The row aborts with the rewrite, survives a crash, and reaches
standbys through WAL.

- Detection: `ProcessUtility_hook` sees `T_RepackStmt` with the concurrently
  option and sets a backend-local flag; `OAT_POST_ALTER` in
  `swap_relation_files` (`repack.c (swap_relation_files)`) supplies the table OID at the swap.
- Census [V]: `CLUOPT_CONCURRENT` is set in exactly one place
  (`repack.c (ExecRepack)`, from the statement option); `cluster_rel` is called only
  from `repack.c (ExecRepack)`, `repack.c (process_single_relation)` and `vacuum.c (vacuum_rel)` (VACUUM FULL,
  never concurrent).
- Empty-table exemption: if the old storage has zero blocks at the swap (under
  AccessExclusiveLock), no row ever lived in it, every snapshot correctly sees
  it empty, and no record is written.
- Refuse REPACK (CONCURRENTLY) if the library was not loaded through
  `shared_preload_libraries`, since other backends would then lack the reader
  check.

**Reader.** Same interception points as above (ExecutorStart after calling
through, ProcessUtility for COPY TO). Per RTE_RELATION with storage, after the
lock is held, snapshot S (in ExecutorStart only for the relations the
executor opened: "Relations the plan does not read", below):

1. Fast path: if the record table has zero blocks, pass.
2. Scan the record table with `SnapshotAny` (deleted rows included) for the
   table OID.
3. Raise `ERRCODE_T_R_SERIALIZATION_FAILURE` if any record's XID is not our
   own transaction, did commit, and is still in progress according to S
   (`XidInMVCCSnapshot`, `snapmgr.c (XidInMVCCSnapshot)`).
4. Parallel workers run the same check; they call `ExecutorStart` themselves
   (`execParallel.c (ParallelQueryMain)`) with the leader's snapshot.

pg_class xmin is not consulted, so GRANT/rename/reloptions cannot cause a
false pass or a false error. The record is keyed by table OID, not
relfilenode, because a later MVCC-safe rewrite copies the unsafe XIDs across
(`rewriteheap.c (rewrite_heap_tuple)`) and the hazard survives it.

**Error text.** The error means "the table's contents as of this snapshot can
no longer be determined": the copy stamps every row with the REPACK XID
(`heapam_handler.c (heap_insert_for_repack)`) and drops row versions deleted after S, so
nothing remains to test visibility against.

    ERROR:  cannot read table "customers" with this snapshot
    DETAIL:  REPACK (CONCURRENTLY) rewrote the table after the snapshot was
             taken, and row visibility for older snapshots was not preserved.
    HINT:  Retry the transaction.

**Accepted spurious error.** A table that has pages but no row visible to S
(only deleted rows dead to everyone, or only rows inserted after S) still
errors, although stock returns a correct empty result. Removing the
"only newer rows" half would need the oldest inserting XID of the old heap,
which costs a second full scan per REPACK (no hook inside the copy) and stops
working once any row is frozen. Rows deleted after S are a real wrong result
and must error.

**Relations the plan does not read (developer decision, 2026-11-23).** In
ExecutorStart the reader checks the range-table relations the executor has
opened, not every RTE_RELATION entry. Reason: the range table also holds
relations the plan never scans, and checking them refused queries that read
no row of the repacked table and that stock PostgreSQL answers correctly: a
table removed as a useless left join (`analyzejoins.c
(remove_useless_joins)` leaves the entry in place), a scan replaced by an
empty Result for constant-false quals, and a partition pruned when the
executor starts (`execPartition.c (ExecDoInitialPruning)`).
`specs/unread_relations.spec` has the three cases and a control for each
that reads the table and is still refused.

Why it is acceptable:

- A plan node reads a range-table relation only through the Relation that
  `execUtils.c (ExecGetRangeTableRelation)` opens and stores in
  `EState.es_relations`. No other executor code opens a range-table entry's
  relation; the other `table_open` calls in `src/backend/executor` are for
  trigger targets, their ancestors and tuple-routing partitions, which are
  not range-table reads.
- Its callers all run while the plan is initialized, inside
  `standard_ExecutorStart`, before the check: `ExecOpenScanRelation` from
  every scan node's init function, `ExecInitResultRelation` from
  `ExecInitModifyTable`, `execMain.c (InitPlan)` for row marks, and the
  partition pruning setup. `execMain.c (EvalPlanQualStart)` initializes its
  plan copy later with the same array and the same pruning result, so it
  opens nothing new.
- When that argument does not apply, every RTE_RELATION entry is checked as
  before: the hooks called before this one left no executor state, or the
  plan contains a CustomScan, whose provider may open range-table relations
  itself (`openRelationsAreAllRead` in `src/pg_repack_safe.c`).

The argument is for PostgreSQL 19's executor. A later major that opens a
range-table relation after ExecutorStart, or by another route, would need
it made again. COPY relation TO is unchanged: it checks the one relation it
names.

**Divergence from principle 1 (AGENTS.md).** The pg_class variant errored on
any storage change it did not recognise. This variant is silent on any rewrite
the writer side did not record, so safety rests on the writer census above.
Reason: the developer ruled spurious errors on MVCC-safe rewrites and new
tables unacceptable (2026-11-15). Remaining fail-open cases:

- library never loaded in the primary backend that runs REPACK: it runs
  unrecorded, and an unloaded library cannot check for itself (a backend
  that loaded it other than through `shared_preload_libraries` refuses,
  step 9; `t/009_not_preloaded.pl` pins the rest);
- library not loaded on a standby: user error, which can give wrong
  results; the WAL record (step 10) stops such a standby's replay, except in
  the case of research question 17;
- readers that bypass the executor and COPY;
- binary-upgrade mode (below);
- the session that runs one of the extension's own scripts (below).

**Binary-upgrade exemption (developer decision, 2026-11-16).** The reader
check refuses a query when the extension exists but its record table cannot
be found. In binary-upgrade mode that refusal is skipped and the query
passes. Reason: pg_dump creates the extension empty with
`binary_upgrade_create_empty_extension` (`pg_dump.c (dumpExtension)`) and adds the
record table in a later statement, so the refusal made `pg_restore` fail and
blocked pg_upgrade of any database with the extension
(`t/003_pg_upgrade.pl` reproduced it). Why it is acceptable: only superusers
can connect in that mode (`postinit.c (InitPostgres)`), the server listens only on
pg_upgrade's private socket (`pg_upgrade/server.c (start_postmaster)`), and no snapshot
survives the upgrade. The refusal for a non-empty record table is not
exempted.

The mode is a postmaster option, not a pg_upgrade private: `-b` sets
`IsBinaryUpgrade` (`postmaster.c (PostmasterMain)`), so an operator can start
any cluster with `pg_ctl start -o "-b"`, and then the exemption lasts as long as that
server runs and applies to TCP connections too, if the configuration allows
them.

The exemption cannot hide an unsafe rewrite, for two reasons:

- The ProcessUtility hook refuses REPACK (CONCURRENTLY) when the extension
  exists and its record table cannot be found, with no binary-upgrade
  exemption (`reader_check` regression test; `t/003_pg_upgrade.pl` on a
  server started with `-b`). The check runs when the statement starts and
  locks the record table, so it is still in place at the swap.
- Stock PostgreSQL refuses to create the new heap in binary-upgrade mode
  unless its OID was preset: "pg_class heap OID value not set when in binary
  upgrade mode" (`heap.c (heap_create_with_catalog)`), seen in `t/003_pg_upgrade.pl`
  when REPACK (CONCURRENTLY) ran under `-b` with the record table present. A
  superuser can preset the OIDs with the `binary_upgrade_set_next_*`
  functions, so this alone is not a guarantee.

**Extension-script exemption (developer decision, 2026-11-23).** The reader
check also passes an extension without its record table, and a record table
without its triggers, in a backend that is running one of the extension's
own scripts: `creating_extension` is set and `CurrentExtensionObject` is
this extension (`extension.c (execute_extension_script)`). Reason: the
install script creates the record table in one command and its triggers in
later ones, and event triggers fire for each command of an extension
script. A `ddl_command_start` or `ddl_command_end` trigger whose function
runs a query therefore reached the reader check in both states, and
`CREATE EXTENSION pg_repack_safe` failed in any database with such a
trigger, a DDL audit trigger for example
(`t/020_create_with_event_trigger.pl` reproduced it).

Why it is acceptable:

- Only the two "cannot be used" errors are skipped. A record table that
  exists is still loaded, and its records are still checked against the
  query's snapshot.
- The script's changes are not committed, so no other backend sees the
  extension without its table or the table without its triggers.
- REPACK (CONCURRENTLY) is not exempt, as in binary-upgrade mode, and it
  cannot run in a transaction block, so it cannot run inside a script.
- During `CREATE EXTENSION` the table is new and holds no record.

PostgreSQL sets the same two variables for `ALTER EXTENSION ... UPDATE`, so
the exemption applies to update scripts too. That is part of the decision:
a future update script that replaces the triggers would otherwise fail the
same way, and the points above hold for it. No update script exists yet
(prerelease), so no test covers that path.

### 6c. Build questions investigated 2026-11-15

Answers from source reading; [R] marks those since confirmed by a probe in
this repo, the rest are [I].

Probe results, 2026-11-15, skeleton library `src/pg_repack_safe.c`:

- **Q13, Q9 [R]** `sql/storage_change.sql`: the hook reports exactly one
  storage replacement for VACUUM FULL, CLUSTER, REPACK, REPACK
  (CONCURRENTLY) in three option spellings, rewriting ALTER TABLE and
  REFRESH MATERIALIZED VIEW; none for `SET (fillfactor)`, RENAME COLUMN, ADD
  COLUMN or TRUNCATE. The old storage's block count reads 1 for a three-row
  table and 0 for an empty one. Only REPACK (CONCURRENTLY) is classified
  unsafe, and the classification does not survive a failed statement.
  CREATE MATERIALIZED VIEW also reports a replacement, of zero blocks.
- **Q5 [R]** `t/002_standby_record_removal.pl`, standby without
  hot_standby_feedback: after the DELETE is replayed the row is invisible to
  a new snapshot but still physically present; replaying the VACUUM cancels
  the older REPEATABLE READ query ("canceling statement due to conflict with
  recovery"); the table returns to zero blocks on both nodes. The test uses a
  plain table of the record table's shape, not a `SnapshotAny` scan in C.
- **Q8 [R, fast path only]** Local debug build (`-O0`, assertions on).
  Timing each step in a loop inside one backend (2M iterations, three runs):
  whole check about 620 ns per `ExecutorStart`, of which opening, locking
  and closing the record table is about 430 ns and reading its size (an
  `lseek`) about 140 ns; the cached OID lookup is 1 ns. A select-only
  prepared pgbench query takes 94 us single-client, so the check is under 1%
  of it. pgbench throughput comparisons (4 clients, 12-15 s rounds) showed
  0-2.4% but were at or below run-to-run noise and are not relied on. Not
  measured: an optimised build, and the slow path while records exist.
  perf was unavailable (`perf_event_paranoid` = 4).
- **Q8 follow-up [I].** Cache "record table is empty" per backend and skip
  the open and size check while it holds (a 2 ns flag test). The writer
  sends a relcache invalidation for the record table when it inserts a
  record; a reader processes it when it acquires its lock on the rewritten
  table, which is the same path by which it learns the new relfilenode
  (`lmgr.c (LockRelationOid)`). The flag starts unset in every new backend and is cleared
  on any invalidation reset. Needs the writer, then an isolation test and a
  standby test that a stale flag cannot pass a reader.

- **Recognising the swap (Q13).** `swap_relation_files` updates pg_class
  (`CatalogTupleUpdateWithInfo`) and fires the hook (`repack.c (swap_relation_files)`) with no
  `CommandCounterIncrement` between. In the hook the syscache still returns
  the pre-swap row, and a `SnapshotSelf` scan of pg_class returns the new one.
  "The two relfilenodes differ" identifies a storage change by meaning, at
  every caller, and is the general detector of §5. Whether the change is
  unsafe comes from the statement flag (only REPACK CONCURRENTLY for now).
  Probe: log both values in the hook for REPACK, VACUUM FULL, GRANT.
- **Old storage size (Q9).** For the same reason the relcache entry of `r1`
  still points at the old storage in the hook, so its block count is readable
  there. Probe: empty and one-row tables.
- **Record lifetime (Q5).** Corrected 2026-11-16, see step 6. A DELETE is
  safe only if the deleting transaction is not the rewriter and its XID
  follows the rewriter's XID R. The reader scans with `SnapshotAny`, so it
  needs the row physically present, not live. On the primary, VACUUM keeps a
  deleted row while its deleter does not precede OldestXmin
  (`heapam_visibility.c (HeapTupleSatisfiesVacuum)`), and a snapshot that does not see R
  has xmin <= R. On a standby, the removal replays with a recovery conflict
  only up to the logged horizon, and that horizon ignores a row deleted by
  its inserting transaction (`heapam.c (HeapTupleHeaderAdvanceConflictHorizon)`): such a removal cancels
  nothing [R, step 6]. A deleter whose XID follows R puts the horizon at
  or after every needing snapshot's xmin, and the conflict cancels that
  snapshot instead (`t/002_standby_record_removal.pl`).
- **Why not shared memory.** A shared-memory record fed by the WAL record's
  redo routine would be cluster-wide and cheap, but a standby or base backup
  that starts replay at a checkpoint between the WAL record and the commit
  never sees the record. Table rows are in the data pages and have no such
  window. Rejected.
- **Reader coverage (Q7).** Inlined SQL functions put their relations in the
  outer query's range table; non-inlined functions run their own
  `ExecutorStart`. `COPY rel TO` opens and locks the relation inside `DoCopy`
  (`copy.c (DoCopy)`) and scans it directly (`copyto.c (CopyRelationTo)`), partitions included
  (`copyto.c (DoCopyTo)`), so the ProcessUtility guard must itself lock the
  relation and its partitions before checking.
- **TOAST and indexes (Q10).** TOAST reads through the table are behind the
  table's check. A direct `SELECT` from the `pg_toast` relation has the TOAST
  OID in its range table, so the writer also records the TOAST OID the table
  has after the swap (REPACK (CONCURRENTLY) swaps TOAST by relation, not by
  content, `repack.c (rebuild_relation_finish_concurrent)`). Indexes
  cannot be named in a query.
- **Database without the extension (Q14).** The writer refuses REPACK
  (CONCURRENTLY) when the record table does not exist in the current
  database. A reader in such a database passes, which is sound only because
  of that refusal.
- **Tampering with the record table (Q15).** *Superseded by step 9: UPDATE
  and DELETE of records are refused by trigger, and VACUUM FULL, CLUSTER and
  REPACK without CONCURRENTLY of the record table are allowed.* As first
  designed: DELETE is harmless (see record
  lifetime). Refuse DROP (`OAT_DROP`), TRUNCATE (`OAT_TRUNCATE`) and any
  rewrite (REPACK, CLUSTER, VACUUM FULL) of the record table; a rewrite drops
  dead rows a standby may still need (research question 1). A superuser UPDATE of
  a record is not defended against.
- **Cost (Q8).** *Superseded by step 7: each backend keeps a copy of the
  records, and a query does not open the record table.* As first designed:
  the fast path is one open-and-size check of the record
  table per `ExecutorStart`, not per relation; see the probe results above
  for its cost and the proposed cache.

## Candidate approaches

| # | Where | Catches | Misses | False positives | Race-free |
| --- | --- | --- | --- | --- | --- |
| A | ProcessUtility at start: refuse if any other backend in the database has an xmin | snapshots existing at start | snapshots taken during the run; standbys; CASCADE / logrep TRUNCATE | refuses nearly always on a busy database | No |
| B | `OAT_POST_ALTER` / `OAT_TRUNCATE` / `XACT_EVENT_PRE_COMMIT`: refuse if any other snapshot exists | everything up to the check, all spellings | snapshots between check and commit; standbys | as A, after all the work | No (small window) |
| C | CIC-style wait before commit under AccessExclusiveLock | victims queued on our lock die by deadlock error | new snapshots during the wait; standbys | waits on unrelated long transactions | No (livelock) |
| D | `XACT_EVENT_COMMIT`: wait and cancel/terminate pre-commit snapshot holders | everything on the primary | standbys | kills unrelated sessions | Yes on primary, but cannot ERROR, interrupts held: unsafe |
| E | Reader side, pg_class xmin check (§6a). **Rejected** | every executor/COPY victim, every rewrite spelling | reader's own DDL after the rewrite; direct-scan extensions; output plugins | MVCC-safe rewrites; tables created after S | Yes |
| F | Core: make CONCURRENTLY preserve xmin/xmax | everything | — | none | Yes (not an extension) |
| G | **Reader side, unsafe-rewrite record (§6b). Chosen** | every executor/COPY victim of a recorded rewrite, primary and standby | unrecorded rewrites; direct-scan extensions; output plugins | non-empty table with no row visible to S | **Yes** |
| H | Writer side, commit then wait: session-level AccessExclusiveLock taken in `OAT_POST_ALTER`, commit, `WaitForOlderSnapshots`, unlock | every reader on the primary, including direct scans | standbys; exposure if the waiter dies; TRUNCATE/ALTER in transaction blocks | none (blocks instead of erroring) | Yes on primary while the waiter lives |
| I | Custom WAL resource manager record at the swap | standbys without the library (replay stops) | — | halts an unprepared standby | n/a |

Notes on H [I]: REPACK (CONCURRENTLY) cannot run in a transaction block
(`repack.c (ExecRepack)`), the hook fires under the lock taken at `repack.c (rebuild_relation_finish_concurrent)
`, the wait list is fetched once and only shrinks (`indexcmds.c (WaitForOlderSnapshots)`), so the
post-commit set of old snapshots is finite and there is no livelock. Session
locks are released on abort (`proc.c (ProcReleaseLocks)`). Cost: t is fully locked until the
oldest pre-commit snapshot ends. H saves no reader that G would error: an
old-snapshot reader that wants t queues on the session lock while H waits for
that reader, which is a deadlock, and it can never read t correctly anyway.
H's only gain over G is blocking readers that bypass the guarded paths.
Proposed on-list by Nikalayeu, 2025-08-09
(history.md §3). Notes on I [I]: an unregistered rmgr fails replay
(`rmgr.c (RmgrNotFound)`), registration needs preload (`rmgr.c (RegisterCustomRmgr)`), and the redo routine
could raise a recovery conflict (`standby.c (ResolveRecoveryConflictWithSnapshot)`).

## Design direction

**Decided by the developer, 2026-11-15**

1. The guarantee is a reader-side error, not a refusal of REPACK.
2. No spurious errors for MVCC-safe rewrites or for tables created after the
   snapshot. This selects the record-based check (G, §6b) over the pg_class
   check (E), and accepts the principle 1 divergence recorded in §6b.
3. Take the zero-blocks empty-table exemption and accept the remaining
   spurious error (§6b) as a documented limitation.
4. Record only REPACK (CONCURRENTLY) for now, but build the writer side as
   the general storage-change detector (§5) so TRUNCATE, rewriting ALTER TABLE
   and REFRESH MATERIALIZED VIEW can be switched on later. Until then they
   keep stock documented behaviour and are listed as known gaps. Before any
   of them is switched on, the writer must handle or refuse PREPARE
   TRANSACTION: unlike REPACK (CONCURRENTLY), they run in a transaction
   block (research question 18).
5. Do not build the commit-then-wait mode (H): it saves no reader that G
   would error and locks the table until old snapshots end. Readers that
   bypass the guarded paths are a known gap with a reproducer. Keep a
   best-effort check before the work starts: refuse REPACK (CONCURRENTLY),
   under a setting, when a snapshot older than a threshold exists. It is racy
   and is not part of the guarantee.
6. Emit the custom WAL record (I) at the swap so a standby without the
   library stops replay instead of returning empty tables. Accepted costs:
   every physical standby, and anything else replaying that WAL, needs the
   library. The resource manager ID is 149, reserved on the PostgreSQL
   wiki.

Correctness argument for G: a wrong result needs a read of t with a snapshot S
that does not see the rewriter's XID R as committed, after the rewrite
committed. That read happens under a lock taken after the rewriter released
AccessExclusiveLock; by then the record (t, R) is committed and visible to a
current snapshot, and S does not see R, so the reader errors before scanning.
Conversely, if every record's XID is visible to S, S sees every row of the
rewritten storage as stock MVCC would. Holds for recorded rewrites read
through the guarded paths.

## Implementation plan

Burn-down list. A step is ticked only when its check passes; steps run in
order unless noted.

- [x] **0. Skeleton and probes.** Library with the three hooks, record table,
  trace setting. Check: `storage_change` regression test and
  `t/002_standby_record_removal.pl` pass (§6c).
- [x] **1. Test harness with the library preloaded.** The guard needs
  `shared_preload_libraries`, which the developer's cluster on port 55434
  does not set. Run the isolation specs against a temporary cluster and leave
  that cluster alone. Check: the existing specs pass unchanged there.
  Done: `ISOLATION_OPTS` runs them on a temporary instance with
  `specs/isolation.conf` (now `test/temp_instance.conf`); a probe spec on that instance showed
  `shared_preload_libraries = pg_repack_safe` and `wal_level = replica`. The
  `REGRESS` suite still runs on the configured server without the library
  preloaded.
- [x] **2. Flip the reproducers.** Change the expected output of
  `repack_concurrently_snapshot`, `repack_concurrently_cursor` and
  `t/001_repack_pg_dump.pl` from the wrong rows to the 40001 error (pg_dump:
  a failed dump). Controls stay unchanged. Check: the three tests fail, for
  that reason only.
  Done: the specs create the extension and expect `cannot read table "..."
  with this snapshot` at `r_read` and `c_fetchall`; `t/001` preloads the
  library and expects pg_dump to fail with that message. The run showed the
  stock wrong rows at exactly those two steps and the four GUARDED subtests
  of `t/001` failing; the controls passed. isolationtester prints only the
  primary message (`isolationtester.c (try_complete_step)`) and pg_dump does not print
  the SQLSTATE, so none of the three pins 40001.
- [x] **3. Writer.** At the swap, when the innermost statement is REPACK
  (CONCURRENTLY) and the old storage has blocks, insert (table OID, XID) and
  the post-swap TOAST OID into the record table. Check: a regression test
  shows one record after REPACK (CONCURRENTLY), none for an empty table, none
  after VACUUM FULL, none after a failed REPACK. The writer must tell the
  rewritten table from the transient heap by meaning, not by the
  `pg_temp_<oid>` name (`repack.c (make_new_heap)`) the trace NOTICE filters on: a user
  table can carry that name and would get no record. Check: the regression
  test includes a user table named `pg_temp_1`.
  Done (`sql/writer.sql`). The transient heap is the side of the swap whose
  old storage was created in this transaction: that storage is queued for
  deletion at abort (`storage.c (RelationCreateStorage)`; pre-existing storage only ever at
  commit, `storage.c (RelationDropStorage)`), read through `smgrGetPendingDeletes`. The
  relation's own "created here" flag cannot serve: `swap_relation_files`
  copies the target's `rd_createSubid` onto the transient heap
  (`repack.c (swap_relation_files)`), and a probe showed the transient heap recorded under
  that rule. The trace NOTICE uses the same rule (developer decision,
  2026-11-16), so it no longer reports CREATE MATERIALIZED VIEW. The old
  storage's blocks are counted through the relation cache, which still
  points at the old storage until the next CCI (`RelationAssumeNewRelfilelocator`
  only sets subtransaction IDs, `relcache.c (RelationAssumeNewRelfilelocator)`); the writer checks that
  rather than assuming it, and refuses on a mismatch. A table whose rows
  were all deleted is recorded and its new storage is empty, which shows the
  old storage is the one counted. The record table is locked
  (RowExclusiveLock) and its definition checked when the statement starts,
  so no lock is taken mid-swap and the table cannot be dropped, renamed or
  moved before the record is written; `USING heap` fixes its access method.
  Moved here from step 9 (developer decision, 2026-11-16): REPACK
  (CONCURRENTLY) is refused in a database without the extension.
  Until step 4, the placeholder reader refuses every query in a backend with
  the library loaded once a record exists. That is why the isolation specs,
  `t/001`, `t/003` and `t/004` now fail, each on that refusal only; the
  regression suite passes.
- [x] **4. Reader.** Replace the "cannot check" refusal with the
  `SnapshotAny` scan and the error text of §6b. Check: the two isolation
  specs from step 2 pass, and a TAP test pins SQLSTATE 40001, DETAIL and
  HINT (psql with `VERBOSITY verbose`). `t/001_repack_pg_dump.pl` still fails
  here: pg_dump reads with `COPY ... TO stdout` (`pg_dump.c (dumpTableData_copy)`), which
  scans without the executor (`copyto.c (DoCopyTo)`, `copyto.c (CopyRelationTo)`); it passes at
  step 8.
  Done. Both specs pass; `t/005_reader_guard.pl` pins 40001, DETAIL and
  HINT, shows EXPLAIN without ANALYZE passing (it reads no rows, so the check
  is skipped) and EXPLAIN ANALYZE refused, and refuses a direct read of the
  post-swap TOAST relation (Q10). `t/003` passes again; `t/004` now refuses a
  real old-snapshot read instead of a planted record. `t/001` fails only its
  four GUARDED subtests (COPY). The relations checked are every
  `RTE_RELATION` of the plan's range table; the snapshot is the query's
  (`queryDesc->snapshot`), and a matching record under a non-MVCC snapshot
  is refused. The snapshot test runs before the commit-log lookup, so a
  record older than the snapshot's xmin never reaches the commit log.
  `reader_check` keeps the single-session verdicts that must pass
  (bootstrap, committed, aborted and own XIDs); its case "a detached record
  table refuses nothing" no longer observes anything in one session and
  moves to step 9 (Q15). Not covered yet: parallel workers (Q7, step 5).
  `rewrite_xid` is `xid8` (Q19).
- [x] **5. Negative and edge tests.** Interleavings 7, 9, 11-16: no error
  after VACUUM FULL alone, GRANT alone, a table created after the snapshot,
  or an empty table; error after own DDL, after VACUUM FULL following REPACK,
  and when rows were deleted after the snapshot; the accepted spurious error
  pinned. Check: all pass.
  Done (`specs/reader_guard_edges.spec`, 14 permutations, all as expected).
  Added: a record for another table refuses nothing; reads through a view,
  an inlined set-returning SQL function, a parallel plan
  (`debug_parallel_query`) and a partitioned parent are refused (Q7); a
  table created after the snapshot and then repacked errors (a second form
  of the accepted spurious error). Run on stock PostgreSQL 19 without the
  library, every permutation expected to error returns 0 rows for a 5-row
  table, and both spurious-error permutations return a correct 0.
- [x] **6. Record cleanup.** DELETE records whose XID precedes every local
  snapshot; rely on VACUUM for removal. Check: the record table returns to
  zero blocks after old snapshots end; a standby test with the real table
  and the library loaded on both nodes.
  Outcome (developer decision, 2026-11-16): **no cleanup for now**; records
  stay live and accumulate, a known limitation in the README, to be
  revisited after step 7's cache. The first design, chosen first, had the
  writer delete each record at pre-commit of its own transaction and set
  autovacuum to run on any dead row. Its test passed on the primary (the
  record survived VACUUM while an old snapshot lived; autovacuum emptied the
  table afterwards) but caught a silent wrong result on a standby without
  hot_standby_feedback: a standby snapshot from before the REPACK read the
  table as empty (`after|0`), with no recovery conflict. Cause:
  `HeapTupleHeaderAdvanceConflictHorizon` (`heapam.c (HeapTupleHeaderAdvanceConflictHorizon)`) leaves a
  row deleted by its inserting transaction out of the conflict horizon, so
  its removal cancels no standby query. The design was reverted. A later
  cleanup must delete from another transaction whose XID follows the
  record's (options weighed: deleting at the next REPACK plus a SQL
  function, or a dynamic background worker). Kept:
  `t/007_standby_reader.pl`, which needs a standby snapshot from before the
  REPACK to get the error after the primary vacuumed the record table; the
  standby check of this step.
  Probe (2026-11-22, before planning bounded records; `pg_waldump` of
  `t/007`'s primary and a diagnostic copy of the test, nothing changed):
  - A plain `VACUUM` of the record table that sets its page all-visible
    logs `PRUNE_VACUUM_SCAN snapshotConflictHorizon: N`, N being the newest
    record's inserting transaction (`pruneheap.c (heap_page_prune_and_freeze)`, `:1960-1962`),
    although it removes nothing.
  - Replayed on a standby without `hot_standby_feedback`, that record
    terminates an idle REPEATABLE READ session whose snapshot xmin is N
    (`FATAL: terminating connection due to conflict with recovery`;
    `confl_snapshot` = 1), after `max_standby_streaming_delay`. So old
    standby snapshots are already ended by a vacuum of the record table
    today, whether or not they read a repacked table.
  - `t/007` passes only because it does not wait for that replay:
    `wait_for_catchup` took the primary's write position as its target,
    `0/0302BCD8`, which was the start of the prune record itself. Its
    "after the REPACK and a VACUUM" assertion therefore runs before the
    VACUUM is replayed. With a two-second wait the reader is gone.
  - The PG19 build has no injection points (`USE_INJECTION_POINTS` is
    undefined in `pg_config.h`), so the concurrent-deleter path cannot be
    forced that way.
  Outcome (developer rulings, 2026-11-22): **records are deleted by the
  next writer.** At its swap, after writing its own records, each
  REPACK (CONCURRENTLY) deletes every committed record whose transaction
  precedes its own (`deleteOlderRecords`), with `heap_delete` called
  directly, not waiting and not decoded; `pg_repack_safe_prune_records()`
  does the same on request. The deleter is never the inserter, so the
  removal carries a conflict horizon. Nothing decides whether a record is
  still needed: VACUUM does, on the primary by its horizon and on a standby
  by the conflict.
  - Rewrites of the record table (ruling 1, option B of three): VACUUM FULL,
    CLUSTER and REPACK drop a deleted version with no conflict horizon, so
    they are refused while the old storage holds one, with a hint to run
    plain VACUUM first. Refusing them always (A) would have made the
    database-wide forms fail in every database with the extension; a WAL
    record whose replay raises the conflict (C) was left for the day the
    user-table gap of `t/012` is taken on.
  - Removal (ruling 2): prune, VACUUM to zero blocks, DROP EXTENSION
    without the override; no per-standby check.
  - Standby cost (ruling 3): accepted and documented in the README's known
    gaps. The probe above showed it already existed for any vacuum of the
    record table; deleting records makes autovacuum visit sooner.
  - Autovacuum (ruling 4): defaults, no reloptions.
  - Checks: `t/017_standby_record_cleanup.pl` (real table, both nodes;
    old standby snapshots get the error, are cancelled or are terminated,
    never a row count), `specs/record_prune.spec`, `sql/records_prune.sql`,
    `sql/writer.sql` (bounded after repeated repacks), `t/003` (a deleted
    record crosses pg_upgrade). Mutation run once: with the reader made to
    skip deleted records, `t/017` reported an old standby snapshot reading
    0 rows and `record_prune` failed; restored.
  - Not exercised by any test: a record deleted by another transaction
    that commits between the deleter's snapshot and its `heap_delete`
    (`TM_Deleted`; no injection points). The code leaves such a row alone.
    A REPACK started while a prune transaction is open waits for it, as it
    waits for any writing transaction, so the two do not meet mid-delete.
- [x] **7. Cache.** Per-backend "known empty" flag and record hash, cleared
  only by invalidation of the record table (writer and cleanup send it).
  Check: an isolation test and a standby test in which a session with a
  warm cache still errors after a later REPACK; the loop timing of §6c shows
  the per-query cost near the flag test.
  Done. Each backend caches every (relid, rewrite_xid) row version of the
  record table, loaded with `SnapshotAny`; each query still judges the
  cached XIDs against its own snapshot, so the cache holds no verdicts. It
  is cleared by a relcache invalidation of the record table: the writer
  queues one (`CacheInvalidateRelcache`), sent at commit before locks are
  released (`xact.c (CommitTransaction)`) and processed by a reader when it takes its
  lock on the rewritten table (`lmgr.c (LockRelationOid)`); changes made by hand send
  one through a statement trigger on the record table, enabled ALWAYS so
  that `session_replication_role = replica` does not skip it. A load cut
  short by an error is never used. The record table OID cache's pg_class
  callback now matches only the record table's (name, schema) hash or a
  reset, instead of any pg_class change; with the coarse callback, any DDL
  in the database reloaded every backend's cache, and REPACK's own pg_class
  changes reloaded it, which hid a missing writer invalidation from the
  tests. Checks: `specs/reader_cache.spec` (cache loaded empty, cache
  holding an earlier record, a record inserted by hand, and the same under
  `session_replication_role = replica`) and `t/007_standby_reader.pl` (a
  warm standby session). Mutations: with the writer's invalidation removed,
  both REPACK permutations of the spec read 0 rows and both standby phases
  fail; with `ENABLE ALWAYS` removed, the replica permutation fails. The
  standby test runs its warm phase before any VACUUM of the record table,
  and with autovacuum off: VACUUM's statistics update of the record table
  carries its own invalidation, which reloaded the cache and masked the
  mutation. Timing was measured with pgbench rather than the in-backend
  loop: select-only, 1 client, -O0 build, about 11,900 tps without the
  library; with the cache, 11,770 to 11,820 tps whether the record table
  is empty, holds records for another table, or holds records for the table
  read; without it (HEAD), 11,280 tps with records for the table read, about
  4.6 us more per query.
- [x] **8. COPY guard.** ProcessUtility locks the relation and its partitions
  before checking. Check: interleaving 8, plus a partitioned table, and
  `t/001_repack_pg_dump.pl` passes.
  Done. For `COPY relation TO` the ProcessUtility hook resolves the
  relation, takes the AccessShareLock DoCopy takes (`copy.c (DoCopy)`) on it and,
  for a partitioned table, on every partition (as `copyto.c (BeginCopyTo)` does), and
  checks each against the active snapshot, which CopyRelationTo scans with
  (`copyto.c (CopyRelationTo)`). It then passes DoCopy a schema-qualified name for the
  relation it checked; the lock keeps that name from being dropped or
  renamed. `COPY (query) TO` and COPY of a relation under row-level
  security run a query and are checked in ExecutorStart. `t/001` passes.
  `t/008_copy_guard.pl`: interleaving 8 for each spelling (plain, column
  list, qualified with options, partitioned parent, `COPY (query)`,
  row-level security), a table without a record, and a READ COMMITTED COPY
  whose statement snapshot predates a REPACK it then waits behind for its
  lock. With the COPY check removed, every relation-COPY case fails, and the
  READ COMMITTED COPY returns no rows and no error, the stock wrong result;
  `COPY (query)` and row-level security still pass through ExecutorStart.
  Partition list (hook audit, 2026-11-20): DoCopy lists the partitions
  again (`copyto.c (BeginCopyTo)`) and scans that list (`copyto.c (DoCopyTo)`), and ATTACH
  PARTITION needs only ShareUpdateExclusiveLock on the parent
  (`tablecmds.c (AlterTableGetLockLevel)`), so a repacked table attached between the two
  listings was scanned unchecked. The guard now takes
  ShareUpdateExclusiveLock on every partitioned table of the tree before
  the listing it checks, and releases those locks when the statement ends.
  Rows enter `pg_inherits` for tables only through DefineRelation
  (`tablecmds.c (StoreCatalogInheritance1)`, parent lock `tablecmds.c (DefineRelation)`), ATTACH PARTITION
  (`tablecmds.c (ATExecAttachPartition)`) and ALTER TABLE ... INHERIT (`tablecmds.c (ATExecAddInherit)`,
  not allowed on a partitioned parent). Cost:
  ATTACH PARTITION, ANALYZE and another `COPY ... TO` of the same
  partitioned table wait for the COPY. `specs/copy_attach_race.spec` pins
  the interleaving; without the lock the ATTACH completes during the COPY,
  which then returns without an error.
  Replaced (2026-11-21): the lock applied to every partitioned COPY on a
  server with the library loaded, also in databases without the extension,
  and could not be skipped when no record exists, because a record written
  after the check (REPACK, then ATTACH, while the COPY waits for a
  partition's lock) must be seen too. The hook now turns
  `COPY partitioned TO` into `COPY (SELECT columns FROM partitioned) TO`,
  as stock does for row-level security (`copy.c (DoCopy)`): the planner
  lists the partitions once and `ExecutorStart` checks what the plan scans.
  The column list comes from `CopyGetAttnums`, so dropped and generated
  columns are left out and the relation form's column errors are kept.
  Probed before building: byte-identical output in text, csv and binary
  (dropped, stored and virtual generated columns, a partition with another
  column order, a partitioned partition); about 4-7% slower on 4 million
  rows; a foreign-table partition is read where the relation form errors;
  pg_dump copies leaves only; tablesync copies a partitioned root in the
  relation form (`tablesync.c (copy_table)`). `specs/copy_attach_race.spec`
  now expects the ATTACH not to wait and the COPY to read the partitions it
  planned; `sql/copy_partitioned.sql` pins the output and errors.
- [x] **9. Refusals.** REPACK (CONCURRENTLY) refused when the library is not
  preloaded (the refusals for a missing extension, a missing record table
  and a redefined record table exist, step 3 and §6b); DROP, TRUNCATE and
  rewrites of the record table refused. Check: regression tests for each.
  Done, with the developer's decisions of 2026-11-16:
  - The REGRESS suite runs on a temporary preloaded instance, like the
    isolation specs (`test/temp_instance.conf`); no suite uses the
    developer's cluster.
  - REPACK (CONCURRENTLY) is refused unless `_PG_init` ran during
    `process_shared_preload_libraries` (`t/009_not_preloaded.pl`: LOAD and
    `session_preload_libraries` refused). A backend that never loads the
    library cannot be stopped: REPACK does not fire event triggers
    (`cmdtaglist.h (PG_CMDTAG)`), so the extension cannot make it load the library.
    That gap is pinned in the same test.
  - While the record table has any block: OAT_DROP of the record table,
    of either of its triggers or of the extension, by any route, and
    OAT_TRUNCATE of the table, are refused unless a superuser sets
    `pg_repack_safe.allow_unprotected_drop`; the message names the set
    (extension, record table, triggers) and the DETAIL the object reached,
    since DROP EXTENSION reaches a trigger first, and the HINT points to
    the README's removal procedure, whose standby check `t/007` exercises.
    Always refused: a storage replacement of the record table other than
    by VACUUM (FULL), CLUSTER or REPACK without CONCURRENTLY (developer
    decision, 2026-11-16: those keep every record with its xmin and value,
    so database-wide VACUUM FULL and REPACK keep working); that leaves
    REPACK (CONCURRENTLY) and ALTER TABLE rewrites, which re-stamp rows and
    can change values (ALTER COLUMN ... USING); UPDATE and DELETE of records,
    by a BEFORE statement trigger enabled ALWAYS (MERGE and writable CTEs
    included); any change that leaves either trigger not firing ALWAYS
    (OAT_POST_ALTER on the trigger, `trigger.c (EnableDisableTrigger)`). The triggers are
    identified by their functions, so renaming them is allowed.
  - The definition check now requires both triggers firing ALWAYS, so a
    record table damaged by a route no hook stops (a backend without the
    library) refuses every read and REPACK (CONCURRENTLY). Exemption,
    extending the binary-upgrade decision of §6b: readers in binary-upgrade
    mode skip the trigger requirement, because pg_restore creates the
    triggers after the table and runs queries in between;
    `t/003_pg_upgrade.pl` fails without it, on that error. REPACK
    (CONCURRENTLY) is not exempt.
  - `sql/protection.sql` covers each refusal, the override and its
    superuser-only setting, and the fail-closed state after a trigger is
    dropped while no record exists. Tests that remove the record table on
    purpose (`reader_check`, `writer`, `storage_change`, `t/006`) set the
    override.
- [x] **10. Standby enforcement.** Custom WAL record at the swap
  (resource manager ID 149). Check: a TAP test in which a standby
  without the library stops replay, and one with it errors the old-snapshot
  reader. Research question 17 is settled or written up as a known gap.
  Done. The library registers a resource manager (ID 149,
  name `pg_repack_safe`, a no-op redo) when preloaded; registering is only
  possible then (`rmgr.c (RegisterCustomRmgr)`). The writer logs one record per unsafe swap,
  before the table's record is inserted. `t/010_standby_wal_record.pl`: a
  standby without the library serves reads until the REPACK, then its
  startup process fails with "resource manager with ID 149 not registered"
  (`rmgr.c (RmgrNotFound)`) and the server shuts down; pg_waldump on the primary shows
  the record in the REPACK's transaction, before its commit; a standby with
  the library errors its old snapshot; the primary's crash recovery replays
  the record. Before the record existed, the standby without the library
  never stopped. Research question 17 was first recorded as a known gap
  (developer decision, 2026-11-16), and reclassified on 2026-11-23 as wrong
  results under misconfiguration; see the question.
- [x] **11. Advisory pre-check.** Refuse REPACK (CONCURRENTLY), under a
  setting, when a snapshot older than a threshold exists. Check: regression
  and isolation tests; documented as best-effort.
  Done (developer decisions, 2026-11-16). Its purpose is availability: the
  reader check already turns every hazard into a 40001, and the pre-check
  spares long transactions that error by refusing the REPACK. Setting
  `pg_repack_safe.precheck_snapshot_age` (ms, -1 off by default, 0 any age).
  Checked at the start and again at the swap. A client session in this
  database counts if it holds a snapshot (`backend_xmin`), the snapshot
  outlives a statement (idle in transaction, or `xact_start` before the
  current statement's start), it holds no lock on the table (REPACK waits
  for those; at the swap none can), and its transaction is at least that
  old. Single statements are not counted: they lock their plan's relations
  when they start. pg_dump locks every dumped table right after its
  snapshot (`pg_dump.c (getTables)`), so it is waited for, not counted.
  Over-refusal, documented: later statements of a READ COMMITTED
  transaction count, since the isolation level is not visible. Not seen:
  standbys, background workers, snapshots after the check.
  `specs/precheck.spec` (start, held lock, swap, threshold, off) and
  `t/011_precheck.pl` (a single long statement not counted, a later
  statement counted, the threshold); removing the spans-a-statement rule
  makes `t/011` fail.
- [x] **12. Known gaps and Group B.** A reproducer and a README entry for
  each: TRUNCATE, rewriting ALTER TABLE, readers that bypass the executor
  and COPY, and research questions 1-4, 6 and 16.
  Done. `specs/stock_rewrites.spec` pins TRUNCATE and a rewriting ALTER TABLE
  (old snapshot reads 0 rows) and REFRESH MATERIALIZED VIEW (Q4, Q16: reads
  the new contents; the refresh inserts frozen rows, `matview.c (transientrel_startup)`;
  REFRESH CONCURRENTLY is the control). `specs/reader_guard_edges.spec`
  pins the foreign-key fast path, a limit of the guard itself.
  Census of readers outside the executor and COPY: every user-table scan in
  `src/backend` outside the executor either takes a fresh snapshot after
  its lock (`tablecmds.c (ATRewriteTable)`; `tid.c (currtid_internal)`; `partbounds.c (check_default_partition_contents)`;
  `index.c (IndexCheckExclusion)`), uses a dirty snapshot (`execReplication.c (RelationFindReplTupleByIndex)`),
  or feeds statistics (`selfuncs.c`, `analyze.c`), except the PostgreSQL 19
  foreign-key fast path (`ri_triggers.c (ri_FastPathCheck)`), which scans the
  referenced table's index with the transaction snapshot: after a REPACK
  (CONCURRENTLY) of that table, an old snapshot's INSERT fails with a
  foreign-key violation (23503) instead of 40001. Not wrong rows; pinned as
  a known gap. Q1, Q2, Q3 and Q6: see the questions. README "Known gaps"
  lists each with its test.
- [x] **13. Productize.** README guarantees and non-guarantees, installation
  and standby requirements, `doc_examples`, hook audit, an optimised-build
  benchmark, the resource manager ID claim.
  - Resource manager ID (2026-11-21): 149, reserved for `pg_repack_safe` on
    the PostgreSQL wiki (CustomWALResourceManagers); the library registers
    it in place of `RM_EXPERIMENTAL_ID`.
  - Hook audit (2026-11-16): no blockers. Fixed at the developer's choice:
    `CHECK_FOR_INTERRUPTS` in `lockRecordTableForWriter`'s retry loop; the
    `object_access_hook` chain is now tested (`t/015_oat_hook_chain.pl`,
    with the test-only module `test/modules/oat_observer`, installed by
    `make installcheck` only; removing our chain call fails it); the
    ordering of ProcessUtility refusals before the previous hook is
    documented in the code and in `doc/installation.md`.
  - Hook audit (2026-11-20): no chain blockers. Fixed: the COPY partition
    list race (step 8). Suggestion, not applied: a `_PG_fini` that restores
    the three hooks. PG19 never calls one: no file under `src/backend` or
    `src/include` refers to `_PG_fini`, and a loaded library is never
    unloaded (`dfmgr.c`), so the function could not run or be tested.
    Restoring a saved hook value would also be wrong whenever a library
    loaded later has chained to ours.
  - README restructured: the problem, the guarantee, what is not
    guaranteed, requirements, how it works. `doc/installation.md` covers
    server configuration (standbys first), settings, `pg_upgrade`, other
    extensions, the resource manager ID, and what `make installcheck`
    installs. `t/010` now also shows a standby restarted with the library
    resumes replay. `doc_examples` covers every SQL example.
  - Optimised build (PostgreSQL 19rc2, meson release, `-O3`, no
    assertions; select-only pgbench, prepared, scale 10, three 10 s runs):
    1 client, 21,090 tps without the library; 20,850 with it and no
    records; 20,725 with records for another table; 20,650 with records for
    the table read (at most about 2%, about 1 us per query). 8 clients:
    71,605 without; 72,400, 72,195, 71,345 with, within run-to-run noise
    (about 1%). Building against the release build also exposed two
    defects the debug build hid, both fixed: PostgreSQL's release headers
    fail `-Wextra -Werror` (unused parameters), so the Makefiles add
    `-Wno-unused-parameter`; and `processUtilityHook` modified `pstmt` and
    `readOnlyTree` before its `PG_TRY` (`-Wclobbered`), so the `PG_TRY`
    moved into `runUtilityAs`, which modifies nothing before its setjmp.
  - Measured again on 2026-11-22, on PostgreSQL 19.0 (meson release,
    `-O3`, no assertions) with the code as of the bounded-records change,
    same pgbench settings, over TCP on localhost, means of three runs. 1
    client: 17,614 tps without the library (runs 16,851 to 18,026); 17,845
    with the library preloaded and no extension; 17,466 with the extension
    and no records; 17,327 with a record for another table; 17,105 with a
    record for the table read (2.9% below the first mean, about 1.7 us per
    query). 8 clients: 61,200; 61,247; 61,328; 61,204; 60,997 (0.3%).
    Three REPACK (CONCURRENTLY) of a 1000-row table, each deleting the
    previous one's record at its swap, took 5.4, 3.4 and 3.0 ms in all.

## Review of 2026-11-21

A code walkthrough of the whole repository on 2026-11-21, with publishing to
the PostgreSQL community as its lens, raised 40 items. Each was fixed or
accepted, and the README, `doc/installation.md` and the code comments now
hold their outcomes. This section keeps what the review decided or measured that is
not written down anywhere else.

**Record table protection.** The README section "How it works" lists the
refusals, and the functions that enforce them state their invariants.

- No index (`protectRecordTableFromIndex`). The motivating probe: with an
  index on the record table, after two more REPACK (CONCURRENTLY) a heap
  scan saw 8 records and an index scan 4, and `max(rewrite_xid)` through the
  index returned an older XID than the newest record. The removal
  procedure's check would have under-reported. [R] Rejected alternative:
  `checkRecordTableDefinition` does not also refuse when the table has an
  index. The reader guard scans the heap, so a stale index cannot make it
  wrong, and refusing every query in the database would be out of
  proportion. A backend that never loaded the library can still create the
  index; that is the never-loaded-session gap the README already lists.
- No column changes (`protectRecordTableColumns`). RENAME COLUMN would be
  harmless on its own. It is refused anyway so that the rule stays one
  sentence: every column change is refused.
- Table constraints are allowed (developer decision). `ADD CHECK` and
  `ADD FOREIGN KEY` are accepted, and the writer's direct `heap_insert`
  does not enforce them. They cannot change a query result. A foreign
  key's `ON DELETE CASCADE` cannot delete records (`sql/protection.sql`).
- Only the writer inserts records (BEFORE INSERT trigger,
  `pg_repack_safe_records_protected`). Before that trigger existed, a probe
  with two clusters and `CREATE PUBLICATION ... FOR ALL TABLES` put a
  publisher's record on a subscriber. There its OID named an unrelated
  table, which refused every SELECT and INSERT ("transaction ID that has
  not been assigned") until the subscriber's XID counter passed the
  record's. Nobody could delete the record.

**COPY of a partitioned table (step 8).** The throughput figure for the
query form (about 4-7% slower) was measured on a debug (`-O0`) build: 4
million rows in 8 partitions to `/dev/null`, three runs. Text took
2357-2403 ms in the relation form and 2468-2488 ms in the query form;
binary took 1567-1568 ms and 1668-1683 ms. It has not been measured on an
optimised build. Logical replication's initial sync of a partitioned root
is the main real user of this path.

**Accepted as they are (developer decisions).**

- A damaged record table (renamed, moved, or missing a trigger) refuses
  even `SELECT 1` in that database. Only its owner or a superuser can damage
  it this way. Utility statements still run, so renaming it back or
  `DROP EXTENSION` recovers (`sql/reader_check.sql`; README "How it works").
- `isConcurrentRepack` is true for any `RepackStmt` with CONCURRENTLY,
  including `CLUSTER (CONCURRENTLY)` and VACUUM FULL forms that
  `repack.c (ExecRepack)` rejects. Such a statement then gets the
  extension's refusal (not preloaded, no extension, pre-check) in place of
  upstream's "option not supported". It fails either way. [V]
- The library is one C file with six concerns (cache, reader, writer, WAL,
  protection, hooks). Whether outside contributors find it readable has not
  been judged.
- `make installcheck` installs the test module `test_record` into the
  server's library directory, and a superuser could use it to write
  arbitrary records. The warning in `doc/installation.md` against running
  installcheck on a production installation is enough.
- On a standby without `hot_standby_feedback`, replaying a VACUUM of the
  record table ends every snapshot in that database older than the latest
  REPACK (CONCURRENTLY) (step 6). The README's figure of "about 50 deleted
  records" before autovacuum runs comes from the default thresholds, not
  from a run.
- No test reaches the branch of `deleteOlderRecords` where `heap_delete`
  does not return TM_Ok. That happens when another cleaner (a second
  REPACK (CONCURRENTLY) or `pg_repack_safe_prune_records()`) deletes the
  same record after this one's scan snapshot was taken. Hitting that window
  needs an injection point, and the local build has none. The branch is
  safe without a test. With `wait = false`, `heap_delete` writes nothing
  for any result but TM_Ok: it returns TM_BeingModified, TM_Updated,
  TM_Deleted or TM_SelfModified before touching the tuple
  (`heapam.c (heap_delete)`). The function then moves on, so the record
  stays exactly as the other transaction leaves it. A record that is not
  deleted protects as before, which is how every record behaved before
  records were deleted at all. So the branch can keep a record longer than
  needed, never remove one early. The other transaction is itself a
  cleaner, since the BEFORE trigger refuses every other DELETE
  (`pg_repack_safe_records_protected`), so its own deletion meets the
  invariant stated in `deleteOlderRecords`' header. If it aborts, the record
  stays until the next cleaner runs. [V]
- The replica-role permutation of `specs/reader_cache.spec` stays,
  although no trigger is involved in it any more: readers learn of a record
  from the writer's own relcache invalidation, which
  `session_replication_role` cannot disable. The permutation fails if
  someone moves that invalidation back into a trigger.

## Research questions

The questions the research raised, all now answered, each with the test that
settles it or the developer's decision. Numbered once and never renumbered:
the README, the specs, the TAP tests and the source cite them as "research
question N".

1. VACUUM FULL / plain REPACK on a standby without hot_standby_feedback: wrong
   results or a conflict cancel?
   **Wrong results**, in stock PostgreSQL 19 too (developer decision,
   2026-11-16: known gap). A standby snapshot misses rows deleted after it
   was taken: the rewrite keeps only what primary snapshots need and logs no
   conflict record (no conflict-horizon code in `rewriteheap.c`,
   `repack.c`, `heapam_handler.c`), and the replayed lock conflicts only
   with queries holding a lock on the table (`high-availability.sgml`).
   `max_standby_streaming_delay = -1` does not help; `hot_standby_feedback`
   does, and one feedback standby protects every standby of that primary.
   Reported on pgsql-hackers by Noah Misch in 2012-01 with this reproducer;
   no fix found. `t/012_standby_safe_rewrite.pl`. Closed as a known gap
   outside the extension's scope (developer decision, 2026-11-23), like 4.
   Option kept, if users ask: record these rewrites too and check the
   records only during recovery. The existing rule is enough there: a
   standby snapshot that sees the rewriter's XID R as committed misses no
   row, since every row the rewrite dropped had a deleter that finished
   before the rewrite's cutoff, so its commit precedes R's in the WAL and
   has been replayed. Costs that kept it unbuilt: spurious errors on
   standbys with `hot_standby_feedback = on`, for which the rewrite is safe
   (decision 2; the cutoff the rewrite used is computed inside
   `repack.c (copy_table_data)` and not passed to the hook, and recomputing
   it at the swap is unsafe because the horizon can move backwards); mapped
   catalogs, whose relfilenode stays 0, would stay uncovered; and one record
   per table for a database-wide VACUUM FULL.
2. Logical-replication tablesync racing REPACK CONCURRENTLY: rows permanently
   missing on the subscriber? **Yes in stock PostgreSQL 19, closed by the
   COPY check.** The sync worker's COPY (`tablesync.c (copy_table)`) with a
   snapshot from before the REPACK returns no rows and no error without the
   check, and is refused with it (`t/013_tablesync.pl`). A real subscription
   rarely reaches it: CREATE SUBSCRIPTION reads the table on the publisher
   and waits behind the REPACK, and a slot created while the REPACK holds an
   XID waits for it.
3. Hot-standby RR query spanning commit replay of REPACK CONCURRENTLY: empty?
   With the library on the standby it gets 40001 (`t/007_standby_reader.pl`,
   `t/017`); without it, replay stops (`t/010`), except in the window of 17.
4. REFRESH MATERIALIZED VIEW: does an old snapshot see the new contents?
   **Yes**: without CONCURRENTLY the refresh inserts its rows frozen
   (`matview.c (transientrel_startup)`, `TABLE_INSERT_FROZEN`). Known gap
   (decision 4), pinned in `specs/stock_rewrites.spec`.
5. Record cleanup: what is the safe deletion rule, when the primary cannot
   see standby snapshots? The deleter must be a later transaction whose XID
   follows the record's; deleting in the rewriter's own transaction gives
   wrong results on standbys (step 6). Each REPACK (CONCURRENTLY) deletes
   the older committed records at its swap, and
   `pg_repack_safe_prune_records()` does the same on request; VACUUM decides
   when they go, by its horizon on the primary and by the recovery conflict
   on a standby (step 6 rulings, 2026-11-22). `t/002`, `t/017`,
   `specs/record_prune.spec`.
6. PL/pgSQL procedure COMMIT loops holding TOAST pointers across a swap:
   values stay correct. PL/pgSQL detoasts before storing a value in
   non-atomic contexts (`pl_exec.c (assign_simple_var)`) and holdable cursors
   detoast what they keep (`portalcmds.c (PersistHoldablePortal)`).
   `t/014_plpgsql_toast.pl`.
7. Reads through parallel workers and inlined SQL functions: checked, as are
   views and partitioned parents (`specs/reader_guard_edges.spec`). Workers
   start at the first `ExecGather` call (`nodeGather.c (ExecGather)`), after
   the leader's check; non-inlined functions run their own `ExecutorStart`
   (`specs/repack_concurrently_cursor.spec`).
8. Overhead of the reader check: at most about 3% for one pgbench client and
   noise for eight, with the per-backend cache (step 7; measured in step 13).
9. Old storage's block count at `OAT_POST_ALTER` in `swap_relation_files`:
   readable there, since `r1`'s relcache entry still points at the old
   storage (§6c; `sql/storage_change.sql`).
10. TOAST and indexes: the writer also records the post-swap TOAST relation,
    so a direct read of it is refused (`t/005_reader_guard.pl`). Indexes
    cannot be named in a query.
11. Moot: approach H is not being built.
12. The own-DDL hole of E: `specs/reader_guard_edges.spec`, interleaving 11.
13. Recognising the swap: the relfilenodes before and after differ (§6c);
    exactly one storage replacement per rewriting command
    (`sql/storage_change.sql`).
14. Database without the extension: REPACK (CONCURRENTLY) is refused there
    (`sql/writer.sql`).
15. REPACK, TRUNCATE or DROP EXTENSION on the record table while records are
    live: refused (step 9; DROP and TRUNCATE unless
    `pg_repack_safe.allow_unprotected_drop` is set by a superuser).
16. REFRESH MATERIALIZED VIEW as a fourth wrong-results path: see 4.
17. A misconfigured standby, one without the library, whose replay starts
    at a checkpoint between the WAL record (I) and the REPACK commit never
    sees the record and does not stop. **User error, not a gap in the
    guarantee** (developer decision, 2026-11-23; recorded as a known gap on
    2026-11-16). The guarantee requires the library on every server that
    replays the WAL; a standby with it checks reads against the records,
    which reach it as table data wherever its replay starts. The record is
    a check for the misconfiguration: it stops a standby without the
    library at the first REPACK (CONCURRENTLY) it replays, except in the
    window this question describes.
    Conditions for a wrong result: a standby without the library, whose
    replay starts at a checkpoint whose redo point lies between a REPACK
    (CONCURRENTLY)'s swap and its commit, reaching consistency before the
    commit, and a snapshot taken there before the commit is replayed that
    then reads the table. Reproducer (manual; needs a build with injection
    points to make deterministic): pause REPACK (CONCURRENTLY) after the
    swap, before commit (for example a debugger breakpoint after
    `swap_relation_files`); run `CHECKPOINT` and take a base backup; start a
    standby from it without the library, open a REPEATABLE READ snapshot
    there before resuming; resume the REPACK; read the table on that
    standby: 0 rows.
    Ways to make the check catch every misconfigured standby, weighed and
    not built:
    - `DELAY_CHKPT_START` from the record to the commit: not possible, the
      commit path asserts the flag is clear and clears it
      (`xact.c (RecordTransactionCommit)`; `DELAY_CHKPT_IN_COMMIT` includes
      the START bit).
    - A second record after the commit: leaves a query between replaying
      the commit and replaying that record.
    - `DELAY_CHKPT_COMPLETE` (2026-11-23, from the source, not prototyped):
      set it in an `XACT_EVENT_PRE_COMMIT` callback before writing the
      record there, clear it at commit or abort. `CreateCheckPoint` does not
      write its checkpoint record while the flag is set
      (`xlog.c (CreateCheckPoint)`), and the commit path asserts only the
      `DELAY_CHKPT_IN_COMMIT` bits, so any checkpoint whose redo point
      follows the record writes its checkpoint record after the commit.
      Every way a hot standby reaches consistency (backup end, a
      restartpoint's `minRecoveryPoint`, the end of local WAL after a crash)
      lies at or after the end of the checkpoint record it started from
      (`xlogrecovery.c (InitWalRecovery)`, `xlog.c (CreateRestartPoint)`),
      so such a standby is consistent only after the commit. Costs: an
      extension writing a core `PGPROC` field; with synchronous replication
      the flag stays set through the commit's wait for the standby
      (`SyncRepWaitForLSN` runs before `XACT_EVENT_COMMIT`), so a checkpoint
      can wait as long as that commit. `pg_rewind` and PITR not checked.
18. PREPARE TRANSACTION: no wrong result in the shipped scope. Settled
    2026-11-23 by the developer from the source argument below, with no test
    (a divergence from principle 2, decided explicitly).
    - REPACK (CONCURRENTLY) cannot be prepared: it calls
      `PreventInTransactionBlock` (`repack.c (ExecRepack)`).
    - The library registers no transaction or subtransaction callback, so
      PREPARE leaves no per-transaction state behind.
    - A prepared transaction reads nothing more and holds no snapshot
      (`twophase.c (MarkAsPreparingGuts)`).
    - `pg_repack_safe_prune_records()` in a transaction that is then
      prepared is a deleter D whose XID follows the records' it deletes,
      which is all `deleteOlderRecords` relies on, whenever D commits. The
      prepared transaction keeps its XID in the proc array until COMMIT
      PREPARED, so VACUUM keeps the rows on the primary, and their removal
      reaches a standby with a conflict horizon of at least D. Readers scan
      with `SnapshotAny`, so a pending delete changes nothing for them;
      `holdsDeletedRecord` counts an uncommitted deleter, so rewrites of the
      record table are refused meanwhile; ROLLBACK PREPARED leaves the
      records in place.
    The part that is still to do, if TRUNCATE or ALTER are ever recorded, is
    a precondition of decision 4.
19. XID wraparound of records. Settled 2026-11-16 (developer decision): the
    column is `xid8`. A record at least 2^31 transactions older than the
    next XID precedes every live snapshot's xmin (XIDs stop short of oldest
    datfrozenxid + 2^31, `varsup.c (SetTransactionIdLimit)`,
    `varsup.c (GetNewTransactionId)`) and passes without a commit-log lookup;
    any other record's 32-bit form is unambiguous; a record at or past the
    next XID is refused as corrupt. `t/006_record_xid8.pl` on a cluster at
    epoch 1; with the age rule disabled the test fails, as a 32-bit column
    would.

## Interleavings that would prove each claim

s1 = REPACK (CONCURRENTLY), s2 = victim, s3 = AccessShareLock blocker. The
numbers are cited by step 5 and the specs. Each one but 10 is a test.

| # | Interleaving | Expected with the guard | Test |
| --- | --- | --- | --- |
| 1 | s2 RR BEGIN, read other → s1 REPACK → s2 count(t) | 40001 (stock: 0) | `repack_concurrently_snapshot`, `t/005` |
| 2 | s2 takes its snapshot after s1 starts, before it commits | 40001: "before commit", not "before start" | covered by 3 (the snapshot is taken while s1 waits at the swap) |
| 3 | s3 locks t → s1 waits → s2 snapshot, then reads t → s3 commits | 40001 (stock: 0) | `t/001` (pg_dump), `t/008` (COPY), `t/013` |
| 4 | RC cursor + STABLE function, REPACK between DECLARE and FETCH | 40001 at the first FETCH | `repack_concurrently_cursor` |
| 5 | s2 touches t first → s1 waits | correct counts, no error | `repack_concurrently_snapshot`, `repack_concurrently_cursor` (controls) |
| 6 | 1 for TRUNCATE and ALTER TABLE ... ALTER COLUMN TYPE | still 0 (decision 4) | `stock_rewrites` |
| 7 | VACUUM FULL instead | correct counts, no error | `reader_guard_edges` |
| 8 | COPY t TO STDOUT after s1 commits | 40001 | `t/008_copy_guard.pl` |
| 9 | GRANT on t after s2's snapshot, no REPACK | no error | `reader_guard_edges` |
| 10 | Writer-side check passes → s2 snapshot → s1 commits → s2 reads 0 | proves A and B are not race-free | none: needs a pause between the check at the swap and the commit, i.e. injection points (§3) |
| 11 | s2 snapshot → s1 REPACK → s2 GRANT on t → s2 count(t) | 40001 (the own-DDL hole of E) | `reader_guard_edges` |
| 12 | s2 snapshot → s1 REPACK → VACUUM FULL t → s2 count(t) | 40001 (the record outlives a safe rewrite) | `reader_guard_edges` |
| 13 | table created after s2's snapshot → s2 count | 0, no error | `reader_guard_edges` |
| 14 | empty table: s2 snapshot → s1 REPACK → s2 count | 0, no error (exemption) | `reader_guard_edges` |
| 15 | s2 snapshot → s3 deletes all rows → s1 REPACK → s2 count | 40001 (exemption not too wide) | `reader_guard_edges` |
| 16 | s2 snapshot → s3 inserts into empty t → s1 REPACK → s2 count | 40001; stock gives a correct 0 (accepted spurious error) | `reader_guard_edges` |

## Mailing-list sources

- [RFC: Making TRUNCATE more "MVCC-safe" (Raudsepp, 2012)](https://postgresql.org/message-id/CABRT9RBRMdsoz8KxgeHfb4LG-ev9u67-6DLqvoiibpkKhTLQfw%40mail.gmail.com)
- [Re: RFC (Noah Misch)](https://postgresql.org/message-id/20120210114205.GA23917%40tornado.leadboat.com)
- [Re: RFC (Robert Haas)](https://www.postgresql.org/message-id/CA%2BTgmobtjc4edN5MY6Z6jzQLp_xsB3gJYAJCB4sqKsg%2BazztJQ%40mail.gmail.com)
- [Re: TRUNCATE SERIALIZABLE and frozen COPY (Simon Riggs)](https://www.postgresql.org/message-id/CA%2BU5nMKX7D6NR%2BVwmkxaueu7Ud-m3y_sy9VkUNhtrgeA6eTt3w%40mail.gmail.com)
- snapshot-too-old removal: commit f691f5b80a8
