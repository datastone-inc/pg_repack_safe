# Preventing wrong results from non-MVCC-safe rewrites: design research

Research notes, 2026-11-15, against the PG19 tree at /opt/pg19/src (19rc2).
Tags: **[V]** read in the code, **[I]** inferred and needs a reproducer,
**[R]** confirmed by this repo's tests. Line numbers marked `~` are within a
few lines. **Status: direction chosen 2026-11-15 (record-based reader guard,
see "Design direction"); step 0 of the implementation plan (skeleton and
probes) is built, the guard itself is not; several points still await the
developer's confirmation.**

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
5. **Refusing REPACK alone is rejected.** Its misses are silent wrong rows, and
   by §2 it would refuse almost always on a busy database.

## 1. Mechanism

**REPACK CONCURRENTLY** [V]
- Takes ShareUpdateExclusiveLock (`repack.c:476-482`), starts the decoding
  worker, copies with the worker's exported historic snapshot
  (`repack.c:1166-1175`, `get_initial_snapshot` 4061;
  `repack_worker.c:157`).
- Copy: `heap_insert_for_repack` → `heap_insert(NewHeap, ...,
  GetCurrentCommandId(true), HEAP_INSERT_NO_LOGICAL)`
  (`heapam_handler.c:2375-2385`, called at 879 and 934). No rewriteheap: xmin
  is not preserved, nothing frozen, every row's xmin is the REPACK XID.
- MVCC scan with the initial snapshot (`heapam_handler.c:662-690`, 763-768);
  recently-dead rows are dropped.
- Concurrent changes replayed in the same transaction (`repack.c:2851-2935`).
- Lock upgrade `LockRelationOid(..., AccessExclusiveLock)` (`repack.c:3476`),
  indexes/TOAST, final replay (3532), index swaps (3547), `finish_heap_swap`
  (3584) → `swap_relation_files` updates pg_class relfilenode (1705, 1857), so
  t's pg_class xmin becomes the REPACK XID.
- The backend gets its XID after the initial snapshot (comment
  `repack.c:314-322`).
- A reader acquiring its lock runs `AcceptInvalidationMessages` (`lmgr.c:~133`)
  and moves to the new relfilenode; old files are unlinked after locks are
  released (`xact.c` CommitTransaction ~2447 / ~2490).

**TRUNCATE** [V]: AccessExclusiveLock up front (`tablecmds.c:1921`), then
`RelationSetNewRelfilenumber` (`tablecmds.c:2272`; `relcache.c:3823`). The
in-place path (`tablecmds.c:2245-2249`) applies only to a relation created or
re-filed in the current subtransaction, which no other snapshot can see.

**ALTER TABLE rewrite** [V]: `ATRewriteTables` → `make_new_heap`
(`tablecmds.c:6042`) → `ATRewriteTable`, scanning with `GetLatestSnapshot()`
(6416), inserting with the current command ID (6227-6229, 6590), then
`finish_heap_swap` (6060). Same effect as REPACK CONCURRENTLY.

**REFRESH MATERIALIZED VIEW** (non-concurrent, not in mvcc-caveats): fills with
`TABLE_INSERT_FROZEN` (`matview.c:494`) and swaps (893). [I] An old snapshot
sees the *new* contents, not an empty view.

**REPACK (plain), CLUSTER, VACUUM FULL: MVCC-safe on the primary** [V]. All go
through `cluster_rel` (`vacuum.c:2311`; CLUSTER is a `RepackStmt`). SnapshotAny
scan keeping RECENTLY_DEAD rows (`heapam_handler.c:789-835`);
`rewrite_heap_tuple` copies xmin/xmax verbatim (`rewriteheap.c:359-366`),
freezing only below FreezeLimit ≤ OldestXmin (`vacuum.c:1194-1199`, 1142;
`procarray.c:1770-1800`). [I] Open: a standby without hot_standby_feedback may
need rows VACUUM FULL dropped as DEAD, with no conflict to cancel its query.

**Other wrong-results modes in REPACK CONCURRENTLY**
- Logical replication initial sync: tablesync uses a USE_SNAPSHOT slot snapshot
  (`walsender.c:1400-1405`) then COPYs; if REPACK commits in between, the
  subscriber's initial copy is empty and the rows are lost for good. [I]
- Hot standby: standby snapshots taken before commit replay see the table
  empty; the primary cannot see them. [I]
- Swap-time lock upgrade (`repack.c:3476`): readers queue behind it and are
  victims if their snapshot predates the commit. [R]
- Serializable: predicate locks promoted at swap (`repack.c:3508-3511`); no
  extra hazard seen. [V]
- TOAST re-toasted with new value OIDs (`repack.c:1466-1500`); open risk is a
  detoast-later pointer held across a procedure COMMIT. [I, low]
- Rejected targets: catalogs, user_catalog_table, TOAST, unlogged/temp,
  matviews, partitioned (`repack.c:994-1080`, 380-395). [V]

## 2. Writer side: detecting "an older snapshot exists"

- Core primitive: `GetCurrentVirtualXIDs` (`procarray.c:3289`), as used by
  `WaitForOlderSnapshots` (`indexcmds.c:439-503`), excluding
  `PROC_IS_AUTOVACUUM | PROC_IN_VACUUM | PROC_IN_SAFE_IC`, waiting with
  `VirtualXactLock`. [V]
- **xmin filtering excludes nothing**: every snapshot existing before our
  commit has xmin ≤ our XID. The real test is "any other backend in this
  database holds any snapshot", which on a busy system is always true.
- `allDbs = false` is right (other databases cannot read the table).
- Slots: an EXPORT_SNAPSHOT/USE_SNAPSHOT walsender appears with a PGPROC xmin
  and `PROC_IN_LOGICAL_DECODING` (`logical.c:299-304`); do not exclude that
  flag, except REPACK's own decoding worker (same lock group).
- hot_standby_feedback gives only an aggregate xmin; standbys without it are
  invisible. **The writer side cannot cover standbys.**
- Prepared transactions hold no snapshot (`twophase.c:470`); not victims.
- WITH HOLD cursors are materialized in `PreCommit_Portals` (`xact.c:~2327`)
  while locks are held: safe. Plain cursors calling functions that open other
  relations later are victims. [R] `specs/repack_concurrently_cursor.spec`.
- Nothing records whether a snapshot will touch t; PGPROC does not even record
  isolation level.

## 3. Timing

- Dangerous set: snapshots taken before the rewriter's `ProcArrayEndTransaction`
  (`xact.c:~2447`) that later read t under a lock acquired after the rewriter
  released AccessExclusiveLock. [V, R]
- A check at ProcessUtility start misses snapshots taken during the copy and
  at the swap. [R]
- No pre-commit check is race-free: snapshots taken between the check and
  commit are dangerous, and an extension cannot hold ProcArrayLock across
  commit. Latest usable points: `XACT_EVENT_PRE_COMMIT` (`xact.c:2338`) and
  `OAT_POST_ALTER` in `swap_relation_files` (`repack.c:1899`), both with a
  window, the latter after all the work.
- `XACT_EVENT_COMMIT` (`xact.c:~2467`) is after commit visibility but before
  lock release; waiting there and cancelling queued victims would close the
  window on the primary, but it runs under `HOLD_INTERRUPTS`, cannot ERROR,
  and cannot use the deadlock detector. Not recommended.
- A CIC-style wait before commit is correct when it ends in a deadlock error
  but can livelock as new snapshots keep appearing. [I]

## 4. Hook points

PG19 `*_hook_type`s: ProcessUtility, ExecutorStart/Run/Finish/End,
ExecutorCheckPerms, post_parse_analyze, planner (+ setup/shutdown), path and
join hooks, stats hooks, object_access (+ `_str`), fmgr, row_security_policy,
emit_log, auth hooks, shmem_request/startup, EXPLAIN hooks. Callbacks:
`RegisterXactCallback`, `RegisterSubXactCallback`,
`CacheRegisterRelcacheCallback`. No REPACK-specific hook; only the test-only
`INJECTION_POINT("repack-concurrently-before-lock")` (`repack.c:3455`).

Writers by parse node: `T_RepackStmt` (`utility.c:863`; REPACK, CLUSTER;
CONCURRENTLY only for `REPACK_COMMAND_REPACK`, `repack.c:281-288`),
`T_VacuumStmt` FULL, `T_TruncateStmt`, `T_AlterTableStmt`,
`T_RefreshMatViewStmt`.

ProcessUtility cannot see: TRUNCATE CASCADE targets (resolved in
`ExecuteTruncateGuts`), logical-replication apply TRUNCATE (`worker.c:3803`
calls `ExecuteTruncateGuts` directly), and which ALTER subcommands rewrite.
Lower-level catch points: `OAT_TRUNCATE` (`tablecmds.c:2460`), `OAT_POST_ALTER`
at every `swap_relation_files`, the `table_rewrite` event trigger (ALTER only).

## 5. TRUNCATE and rewriting ALTER

Same dangerous set. Taking AccessExclusiveLock immediately only protects
readers that already touched t. The record-based guard (§6b) covers them only
if their writers also write a record. Proposed decision 4 leaves them
unrecorded for now. What recording them would take:
- Heap storage is replaced in exactly five places [V]: the two `cluster_rel`
  swaps (`repack.c:1253` non-concurrent, `repack.c:3584` concurrent), ALTER
  rewrite (`tablecmds.c:6060`), REFRESH MATERIALIZED VIEW (`matview.c:895`),
  and TRUNCATE (`RelationSetNewRelfilenumber`, `tablecmds.c:2272`). SET
  TABLESPACE copies blocks and is MVCC-safe.
- TRUNCATE: `OAT_TRUNCATE` fires in `truncate_check_rel` for named and
  CASCADE targets (`tablecmds.c:1985`, 2075) but not for logical-replication
  apply, which never calls `truncate_check_rel` [V: no call in `worker.c`].
  `RelationSetNewRelfilenumber` has no hook of its own.
- ALTER rewrite: no dedicated C hook. `OAT_POST_ALTER` fires from 35 pg_class
  call sites and does not say "this was a swap"; the `table_rewrite` event
  trigger (`tablecmds.c:6022`) is exact but can be disabled, which fails open.
- A general detector ("relfilenode differs at pre-commit from what it was at
  the first hook call, and the command is not a known MVCC-safe one") would
  cover all of them including logical-replication TRUNCATE. [I]

## 6. Reader side

- No relation-open hook. Usable: `ExecutorStart_hook` (call through first so
  locks are held and the range table is final) and `ProcessUtility_hook` for
  COPY TO (`copyto.c:1396` scans with `GetActiveSnapshot()`; lock first, check,
  call through).
- Precedents: `indcheckxmin` (`plancat.c:262`) compares pg_index xmin with the
  snapshot; the 2012 "Making TRUNCATE more MVCC-safe" RFC (relvalidxmin);
  "snapshot too old" (removed in f691f5b80a8, PG17; page-LSN based, not a
  precedent for this).
- Race-freedom (both variants below): every unsafe rewrite holds
  AccessExclusiveLock until commit, and the reader holds at least
  AccessShareLock from the check to the end of its transaction, so no rewrite
  can commit between check and read (`plancache.c:1920-1960`,
  `execUtils.c:851-890`). [V]
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

Nothing here is prototyped; every step is [I] unless tagged.

**Writer.** When REPACK (CONCURRENTLY) swaps a table, insert a row
(table OID, rewriter XID) into an extension-owned table, in the rewriter's own
transaction. The row aborts with the rewrite, survives a crash, and reaches
standbys through WAL.
- Detection: `ProcessUtility_hook` sees `T_RepackStmt` with the concurrently
  option and sets a backend-local flag; `OAT_POST_ALTER` in
  `swap_relation_files` (`repack.c:1899`) supplies the table OID at the swap.
- Census [V]: `CLUOPT_CONCURRENT` is set in exactly one place
  (`repack.c:307`, from the statement option); `cluster_rel` is called only
  from `repack.c:455`, `repack.c:2613` and `vacuum.c:2311` (VACUUM FULL,
  never concurrent).
- Empty-table exemption: if the old storage has zero blocks at the swap (under
  AccessExclusiveLock), no row ever lived in it, every snapshot correctly sees
  it empty, and no record is written.
- Refuse REPACK (CONCURRENTLY) if the library was not loaded through
  `shared_preload_libraries`, since other backends would then lack the reader
  check.

**Reader.** Same interception points as above (ExecutorStart after calling
through, ProcessUtility for COPY TO). Per RTE_RELATION with storage, after the
lock is held, snapshot S:
1. Fast path: if the record table has zero blocks, pass.
2. Scan the record table with `SnapshotAny` (deleted rows included) for the
   table OID.
3. Raise `ERRCODE_T_R_SERIALIZATION_FAILURE` if any record's XID is not our
   own transaction, did commit, and is still in progress according to S
   (`XidInMVCCSnapshot`, `snapmgr.c:1904`).
4. Parallel workers run the same check; they call `ExecutorStart` themselves
   (`execParallel.c:1552`) with the leader's snapshot.

pg_class xmin is not consulted, so GRANT/rename/reloptions cannot cause a
false pass or a false error. The record is keyed by table OID, not
relfilenode, because a later MVCC-safe rewrite copies the unsafe XIDs across
(`rewriteheap.c:359-366`) and the hazard survives it.

**Error text.** The error means "the table's contents as of this snapshot can
no longer be determined": the copy stamps every row with the REPACK XID
(`heapam_handler.c:2375-2385`) and drops row versions deleted after S, so
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

**Divergence from principle 1 (AGENTS.md).** The pg_class variant errored on
any storage change it did not recognise. This variant is silent on any rewrite
the writer side did not record, so safety rests on the writer census above.
Reason: the developer ruled spurious errors on MVCC-safe rewrites and new
tables unacceptable (2026-11-15). Remaining fail-open cases:
- library not preloaded on the primary: REPACK runs unrecorded, and an
  unloaded library cannot check for itself;
- library not loaded on a standby: the record replicates but nothing reads it
  (see candidate I);
- readers that bypass the executor and COPY;
- binary-upgrade mode (below).

**Binary-upgrade exemption (developer decision, 2026-11-16).** The reader
check refuses a query when the extension exists but its record table cannot
be found. In binary-upgrade mode that refusal is skipped and the query
passes. Reason: pg_dump creates the extension empty with
`binary_upgrade_create_empty_extension` (`pg_dump.c:12205`) and adds the
record table in a later statement, so the refusal made `pg_restore` fail and
blocked pg_upgrade of any database with the extension
(`t/003_pg_upgrade.pl` reproduced it). Why it is acceptable: only superusers
can connect in that mode (`postinit.c:941`), the server listens only on
pg_upgrade's private socket (`pg_upgrade/server.c:182`), and no snapshot
survives the upgrade. The refusal for a non-empty record table is not
exempted.

The mode is a postmaster option, not a pg_upgrade private: `-b` sets
`IsBinaryUpgrade` (`postmaster.c:612`), so an operator can start any cluster
with `pg_ctl start -o "-b"`, and then the exemption lasts as long as that
server runs and applies to TCP connections too, if the configuration allows
them.

The exemption cannot hide an unsafe rewrite, for two reasons:
- The ProcessUtility hook refuses REPACK (CONCURRENTLY) when the extension
  exists and its record table cannot be found, with no binary-upgrade
  exemption (`reader_check` regression test; `t/003_pg_upgrade.pl` on a
  server started with `-b`). The check runs when the statement starts; the
  writer (step 3) must look the table up again at the swap.
- Stock PostgreSQL refuses to create the new heap in binary-upgrade mode
  unless its OID was preset: "pg_class heap OID value not set when in binary
  upgrade mode" (`heap.c:1556-1559`), seen in `t/003_pg_upgrade.pl` when
  REPACK (CONCURRENTLY) ran under `-b` with the record table present. A
  superuser can preset the OIDs with the `binary_upgrade_set_next_*`
  functions, so this alone is not a guarantee.

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
  (`lmgr.c:~133`). The flag starts unset in every new backend and is cleared
  on any invalidation reset. Needs the writer, then an isolation test and a
  standby test that a stale flag cannot pass a reader.

- **Recognising the swap (Q13).** `swap_relation_files` updates pg_class
  (`repack.c:~1855`) and fires the hook (`repack.c:1899`) with no
  `CommandCounterIncrement` between. In the hook the syscache still returns
  the pre-swap row, and a `SnapshotSelf` scan of pg_class returns the new one.
  "The two relfilenodes differ" identifies a storage change by meaning, at
  every caller, and is the general detector of §5. Whether the change is
  unsafe comes from the statement flag (only REPACK CONCURRENTLY for now).
  Probe: log both values in the hook for REPACK, VACUUM FULL, GRANT.
- **Old storage size (Q9).** For the same reason the relcache entry of `r1`
  still points at the old storage in the hook, so its block count is readable
  there. Probe: empty and one-row tables.
- **Record lifetime (Q5).** The primary may DELETE a record once its XID
  precedes every local snapshot's xmin; a DELETE is always safe because the
  reader scans with `SnapshotAny`. Physical removal is VACUUM's, which on the
  primary waits for old snapshots and on a standby raises the usual recovery
  conflict (cancel or replay delay) for snapshots that still need the row.
  Probe: standby RR query across DELETE + VACUUM of the record.
- **Why not shared memory.** A shared-memory record fed by the WAL record's
  redo routine would be cluster-wide and cheap, but a standby or base backup
  that starts replay at a checkpoint between the WAL record and the commit
  never sees the record. Table rows are in the data pages and have no such
  window. Rejected.
- **Reader coverage (Q7).** Inlined SQL functions put their relations in the
  outer query's range table; non-inlined functions run their own
  `ExecutorStart`. `COPY rel TO` opens and locks the relation inside `DoCopy`
  (`copy.c:126`) and scans it directly (`copyto.c:1396`), partitions included
  (`copyto.c:1356`), so the ProcessUtility guard must itself lock the
  relation and its partitions before checking.
- **TOAST and indexes (Q10).** TOAST reads through the table are behind the
  table's check. A direct `SELECT` from the `pg_toast` relation has the TOAST
  OID in its range table, so the writer also records the TOAST OID the table
  has after the swap (REPACK (CONCURRENTLY) swaps TOAST by relation, not by
  content, `repack.c:3584`). Indexes
  cannot be named in a query.
- **Database without the extension (Q14).** The writer refuses REPACK
  (CONCURRENTLY) when the record table does not exist in the current
  database. A reader in such a database passes, which is sound only because
  of that refusal.
- **Tampering with the record table (Q15).** DELETE is harmless (see record
  lifetime). Refuse DROP (`OAT_DROP`), TRUNCATE (`OAT_TRUNCATE`) and any
  rewrite (REPACK, CLUSTER, VACUUM FULL) of the record table; a rewrite drops
  dead rows a standby may still need (open question 1). A superuser UPDATE of
  a record is not defended against.
- **Cost (Q8).** The fast path is one open-and-size check of the record
  table per `ExecutorStart`, not per relation; see the probe results above
  for its cost and the proposed cache.

## Candidate approaches

| # | Where | Catches | Misses | False positives | Race-free |
|---|---|---|---|---|---|
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
(`repack.c:324`), the hook fires under the lock taken at `repack.c:3476`, the
wait list is fetched once and only shrinks (`indexcmds.c:~445`), so the
post-commit set of old snapshots is finite and there is no livelock. Session
locks are released on abort (`proc.c:905`). Cost: t is fully locked until the
oldest pre-commit snapshot ends. H saves no reader that G would error: an
old-snapshot reader that wants t queues on the session lock while H waits for
that reader, which is a deadlock, and it can never read t correctly anyway.
H's only gain over G is blocking readers that bypass the guarded paths.
Proposed on-list by Nikalayeu, 2025-08-09
(history.md §3). Notes on I [I]: an unregistered rmgr fails replay
(`rmgr.c:93`), registration needs preload (`rmgr.c:118`), and the redo routine
could raise a recovery conflict (`standby.c:470`).

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
   keep stock documented behaviour and are listed as known gaps.
5. Do not build the commit-then-wait mode (H): it saves no reader that G
   would error and locks the table until old snapshots end. Readers that
   bypass the guarded paths are a known gap with a reproducer. Keep a
   best-effort check before the work starts: refuse REPACK (CONCURRENTLY),
   under a setting, when a snapshot older than a threshold exists. It is racy
   and is not part of the guarantee.
6. Emit the custom WAL record (I) at the swap so a standby without the
   library stops replay instead of returning empty tables. Accepted costs:
   every physical standby, and anything else replaying that WAL, needs the
   library; a resource manager ID must be claimed on the PostgreSQL wiki
   before the first release (`RM_EXPERIMENTAL_ID` during development).

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
  `specs/isolation.conf`; a probe spec on that instance showed
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
  primary message (`isolationtester.c:1024-1031`) and pg_dump does not print
  the SQLSTATE, so none of the three pins 40001.
- [ ] **3. Writer.** At the swap, when the innermost statement is REPACK
  (CONCURRENTLY) and the old storage has blocks, insert (table OID, XID) and
  the post-swap TOAST OID into the record table. Check: a regression test
  shows one record after REPACK (CONCURRENTLY), none for an empty table, none
  after VACUUM FULL, none after a failed REPACK. The writer must tell the
  rewritten table from the transient heap by meaning, not by the
  `pg_temp_<oid>` name (`repack.c:1324`) the trace NOTICE filters on: a user
  table can carry that name and would get no record. Check: the regression
  test includes a user table named `pg_temp_1`.
- [ ] **4. Reader.** Replace the "cannot check" refusal with the
  `SnapshotAny` scan and the error text of §6b. Check: the two isolation
  specs from step 2 pass, and a TAP test pins SQLSTATE 40001, DETAIL and
  HINT (psql with `VERBOSITY verbose`). `t/001_repack_pg_dump.pl` still fails
  here: pg_dump reads with `COPY ... TO stdout` (`pg_dump.c:2429`), which
  scans without the executor (`copyto.c:1362`, `copyto.c:1396`); it passes at
  step 8.
- [ ] **5. Negative and edge tests.** Interleavings 7, 9, 11-16: no error
  after VACUUM FULL alone, GRANT alone, a table created after the snapshot,
  or an empty table; error after own DDL, after VACUUM FULL following REPACK,
  and when rows were deleted after the snapshot; the accepted spurious error
  pinned. Check: all pass.
- [ ] **6. Record cleanup.** DELETE records whose XID precedes every local
  snapshot; rely on VACUUM for removal. Check: the record table returns to
  zero blocks after old snapshots end; a standby test with the real table
  and the library loaded on both nodes.
- [ ] **7. Cache.** Per-backend "known empty" flag and record hash, cleared
  only by invalidation of the record table (writer and cleanup send it).
  Check: an isolation test and a standby test in which a session with a
  warm cache still errors after a later REPACK; the loop timing of §6c shows
  the per-query cost near the flag test.
- [ ] **8. COPY guard.** ProcessUtility locks the relation and its partitions
  before checking. Check: interleaving 8, plus a partitioned table, and
  `t/001_repack_pg_dump.pl` passes.
- [ ] **9. Refusals.** REPACK (CONCURRENTLY) refused when the library is not
  preloaded or the extension is missing in the database (the refusal for
  a missing record table exists, §6b); DROP, TRUNCATE and
  rewrites of the record table refused. Check: regression tests for each.
- [ ] **10. Standby enforcement.** Custom WAL record at the swap
  (`RM_EXPERIMENTAL_ID` for now). Check: a TAP test in which a standby
  without the library stops replay, and one with it errors the old-snapshot
  reader. Open question 17 is settled or written up as a known gap.
- [ ] **11. Advisory pre-check.** Refuse REPACK (CONCURRENTLY), under a
  setting, when a snapshot older than a threshold exists. Check: regression
  and isolation tests; documented as best-effort.
- [ ] **12. Known gaps and Group B.** A reproducer and a README entry for
  each: TRUNCATE, rewriting ALTER TABLE, readers that bypass the executor
  and COPY, and open questions 1-4, 6 and 16.
- [ ] **13. Productize.** README guarantees and non-guarantees, installation
  and standby requirements, `doc_examples`, hook audit, an optimised-build
  benchmark, the resource manager ID claim.

## Open questions needing reproducers

1. VACUUM FULL / plain REPACK on a standby without hot_standby_feedback: wrong
   results or a conflict cancel?
2. Logical-replication tablesync racing REPACK CONCURRENTLY: rows permanently
   missing on the subscriber?
3. Hot-standby RR query spanning commit replay of REPACK CONCURRENTLY: empty?
4. REFRESH MATERIALIZED VIEW: does an old snapshot see the new contents?
5. Record cleanup: a record may be deleted once its XID is visible to every
   snapshot, but the primary cannot see standby snapshots without
   hot_standby_feedback. What is the safe deletion rule?
6. PL/pgSQL procedure COMMIT loops holding TOAST pointers across a swap.
7. Are reads through parallel workers and inlined SQL functions checked?
8. Overhead of the ExecutorStart check (pgbench): one record lookup per
   relation per ExecutorStart, and whether it needs a cache.
9. At `OAT_POST_ALTER` in `swap_relation_files`, which OID owns the old
   storage, and can its block count be read there for the empty-table
   exemption?
10. Does a table record also cover reads of its TOAST relation and indexes in
    every guarded path?
11. (Moot: H is not being built.) Can a `ProcessUtility_hook` commit and
    start a transaction after `standard_ProcessUtility` returns from REPACK
    (CONCURRENTLY)?
12. The own-DDL hole in §6a, as a reproducer, to keep the rejection of E on
    record.
13. How does the hook recognise the swap among the other `OAT_POST_ALTER`
    calls during a REPACK?
14. The library is loaded server-wide but the extension is created per
    database: REPACK (CONCURRENTLY) in a database with no record table
    should refuse.
15. REPACK, TRUNCATE or DROP EXTENSION on the record table itself while
    records are live.
16. Is REFRESH MATERIALIZED VIEW in scope as a fourth wrong-results path?

17. An unprepared standby that starts replay at a checkpoint between the WAL
    record (I) and the REPACK commit never sees the record and does not stop.
    Can the window be closed?
18. PREPARE TRANSACTION: irrelevant for REPACK (CONCURRENTLY), which cannot
    run in a transaction block, but the general detector must handle or
    refuse it before TRUNCATE or ALTER are switched on.

How these group:
- **Needed to build the guard:** 5, 7, 8, 9, 10, 13, 14, 15, 17. All but 8
  and 17 have a proposed answer in §6c, each awaiting its probe.
- **Other possible wrong results in stock PG19, found during the research and
  independent of the guard.** Each needs a reproducer to learn whether it is
  real and, if so, whether the guard catches it or it becomes a known gap:
  1, 2, 3, 4, 6, 16.
- **Bookkeeping:** 11 (moot), 12 (regression test for a rejected design).

## Interleavings that would prove each claim

s1 = REPACK (CONCURRENTLY), driven by `injection_points` at
`repack-concurrently-before-lock` (needs a build with injection points; the
local build has them off); s2 = victim; s3 = AccessShareLock blocker.

1. s2 RR BEGIN, read other → s1 REPACK → s2 count(t): 0 stock [R], 40001 with G.
2. s1 paused at the injection point → s2 RR snapshot → s1 commits → s2
   count(t) = 0: proves "before commit, not before start".
3. s3 locks t → s1 waits → s2 snapshot then count(t) queues → s3 commits → s2
   = 0 [R]; 40001 with G.
4. RC cursor + STABLE function: 40001 at first FETCH with G.
5. Negative control: s2 touches t first → s1 waits, correct counts, no error.
6. 1 and 3 for TRUNCATE and ALTER TABLE ... ALTER COLUMN TYPE: still 0 with G
   unless decision 4 is reversed; pins the known gap.
7. VACUUM FULL instead: correct counts, no error with G.
8. COPY t TO STDOUT after s1 commits: 40001 (ProcessUtility path).
9. GRANT on t after s2's snapshot, no REPACK: no error.
10. Writer-side race: check passes → s2 snapshot → s1 commits → s2 reads 0:
    proves A and B are not race-free.
11. s2 snapshot → s1 REPACK → s2 GRANT on t → s2 count(t): 40001 with G (the
    own-DDL hole of E).
12. s2 snapshot → s1 REPACK → VACUUM FULL t → s2 count(t): 40001 with G (the
    record outlives a safe rewrite).
13. Table created after s2's snapshot → s2 count: 0, no error.
14. Empty table (zero blocks): s2 snapshot → s1 REPACK → s2 count: 0, no
    error (exemption).
15. s2 snapshot → s3 deletes all rows → s1 REPACK → s2 count: 40001 (real
    wrong result; proves the exemption is not too wide).
16. s2 snapshot → s3 inserts into the previously empty t → s1 REPACK → s2
    count: 40001, stock gives a correct 0 (the accepted spurious error).

## Mailing-list sources

- [RFC: Making TRUNCATE more "MVCC-safe" (Raudsepp, 2012)](https://postgresql.org/message-id/CABRT9RBRMdsoz8KxgeHfb4LG-ev9u67-6DLqvoiibpkKhTLQfw%40mail.gmail.com)
- [Re: RFC (Noah Misch)](https://postgresql.org/message-id/20120210114205.GA23917%40tornado.leadboat.com)
- [Re: RFC (Robert Haas)](https://www.postgresql.org/message-id/CA%2BTgmobtjc4edN5MY6Z6jzQLp_xsB3gJYAJCB4sqKsg%2BazztJQ%40mail.gmail.com)
- [Re: TRUNCATE SERIALIZABLE and frozen COPY (Simon Riggs)](https://www.postgresql.org/message-id/CA%2BU5nMKX7D6NR%2BVwmkxaueu7Ud-m3y_sy9VkUNhtrgeA6eTt3w%40mail.gmail.com)
- snapshot-too-old removal: commit f691f5b80a8
