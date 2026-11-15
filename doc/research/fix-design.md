# Preventing wrong results from non-MVCC-safe rewrites: design research

Research notes, 2026-11-15, against the PG19 tree at /opt/pg19/src (19rc2).
Tags: **[V]** read in the code, **[I]** inferred and needs a reproducer,
**[R]** confirmed by this repo's tests. Line numbers marked `~` are within a
few lines. **Status: no design chosen yet.**

## Bottom line

1. Old snapshots see an empty table because of **xmin stamping**, not because
   the old file is gone. REPACK CONCURRENTLY and rewriting ALTER TABLE insert
   every row into a new relfilenode with the rewriter's own XID; TRUNCATE
   installs an empty relfilenode.
2. The dangerous snapshots are those taken **before the rewrite commits**, not
   "before repacking started". [R] `t/001_repack_pg_dump.pl` case 2.
3. **A writer-side refusal cannot be both race-free and usable.** At commit
   every other snapshot in the database is a potential victim, and a new one
   can appear between any check the extension can run and the commit.
4. **Recommended: a reader-side guard.** Once a reader holds its lock on a
   relation, error (40001) if the relation's current storage was installed by
   a transaction its snapshot does not see as committed. This is the
   `relcheckxmin` idea rejected for core in 2026-02 (see
   [history.md](history.md) §3).

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
readers that already touched t. The reader-side check covers them unchanged,
including logical-replication TRUNCATE.

## 6. Reader side

- No relation-open hook. Usable: `ExecutorStart_hook` (call through first so
  locks are held and the range table is final) and `ProcessUtility_hook` for
  COPY TO (`copyto.c:1396` scans with `GetActiveSnapshot()`; lock first, check,
  call through).
- Precedents: `indcheckxmin` (`plancat.c:262`) compares pg_index xmin with the
  snapshot; the 2012 "Making TRUNCATE more MVCC-safe" RFC (relvalidxmin);
  "snapshot too old" (removed in f691f5b80a8, PG17; page-LSN based, not a
  precedent for this).
- Algorithm, per RTE_RELATION with storage, after the lock is held, snapshot S:
  1. `x` = xmin of t's current pg_class row (syscache).
  2. Pass if `x` is frozen, is our own transaction, or is committed and visible
     to S.
  3. Otherwise, to avoid false positives from GRANT/rename/reloptions (which
     also rewrite pg_class): read t's pg_class row as of S
     (`systable_beginscan` with S) and compare relfilenode/reltablespace. If
     they differ, or there is no row as of S (table created after S), raise
     `ERRCODE_T_R_SERIALIZATION_FAILURE`.
  4. Skip mapped catalogs (relfilenode 0) and parallel workers (the leader
     checks).
- Race-freedom: every unsafe rewrite holds AccessExclusiveLock until commit,
  and the reader holds at least AccessShareLock from the check to the end of
  its transaction, so no rewrite can commit between check and read
  (`plancache.c:1920-1960`, `execUtils.c:851-890`). [V]
- Costs and gaps:
  - one syscache lookup per relation per ExecutorStart;
  - the reader fails, not the rewrite;
  - needs `shared_preload_libraries` on the primary and every standby;
  - misses readers that bypass the executor and COPY (direct
    `table_beginscan` in other extensions, output plugins reading
    user_catalog_tables);
  - false positives for MVCC-safe rewrites (VACUUM FULL, CLUSTER, plain
    REPACK, SET TABLESPACE) unless allowlisted via a non-replicated
    shared-memory marker.

## Candidate approaches

| # | Where | Catches | Misses | False positives | Race-free |
|---|---|---|---|---|---|
| A | ProcessUtility at start: refuse if any other backend in the database has an xmin | snapshots existing at start | snapshots taken during the run; standbys; CASCADE / logrep TRUNCATE | refuses nearly always on a busy database | No |
| B | `OAT_POST_ALTER` / `OAT_TRUNCATE` / `XACT_EVENT_PRE_COMMIT`: refuse if any other snapshot exists | everything up to the check, all spellings | snapshots between check and commit; standbys | as A, after all the work | No (small window) |
| C | CIC-style wait before commit under AccessExclusiveLock | victims queued on our lock die by deadlock error | new snapshots during the wait; standbys | waits on unrelated long transactions | No (livelock) |
| D | `XACT_EVENT_COMMIT`: wait and cancel/terminate pre-commit snapshot holders | everything on the primary | standbys | kills unrelated sessions | Yes on primary, but cannot ERROR, interrupts held: unsafe |
| E | **Reader side**: ExecutorStart + ProcessUtility(COPY TO) pg_class check | every executor/COPY victim, primary and standby, every rewrite spelling | direct-scan extensions; output plugins | MVCC-safe rewrites unless allowlisted | **Yes** |
| F | Core: make CONCURRENTLY preserve xmin/xmax | everything | — | none | Yes (not an extension) |

## Recommendation (from the research, not yet decided)

1. E as the correctness mechanism: 40001, "relation "t" was rewritten after
   this transaction's snapshot", hint naming the operation where known.
2. Optional writer-side advisory (B-style): WARNING listing pids with older
   snapshots, or ERROR under a GUC. Documented as best-effort.
3. Require `shared_preload_libraries`; check it at REPACK time on the primary;
   document the standby requirement.

Correctness argument: a wrong result needs a read of t with a snapshot S that
does not see the rewriter's XID as committed, after the rewrite committed. That
read happens under a lock taken after the rewriter released AccessExclusiveLock;
at that point t's pg_class xmin is the rewriter's XID, which S does not see, so
the reader errors before scanning. Conversely, if pg_class xmin is visible to S,
no relfilenode change happened after S. Holds for reads through the guarded
paths.

## Open questions needing reproducers

1. VACUUM FULL / plain REPACK on a standby without hot_standby_feedback: wrong
   results or a conflict cancel?
2. Logical-replication tablesync racing REPACK CONCURRENTLY: rows permanently
   missing on the subscriber?
3. Hot-standby RR query spanning commit replay of REPACK CONCURRENTLY: empty?
4. REFRESH MATERIALIZED VIEW: does an old snapshot see the new contents?
5. Historic pg_class lookup on a standby, including a cancel when catalog rows
   were pruned.
6. PL/pgSQL procedure COMMIT loops holding TOAST pointers across a swap.
7. Are reads through parallel workers and inlined SQL functions checked?
8. Overhead of the ExecutorStart check (pgbench).

## Interleavings that would prove each claim

s1 = REPACK (CONCURRENTLY), driven by `injection_points` at
`repack-concurrently-before-lock` (needs a build with injection points; the
local build has them off); s2 = victim; s3 = AccessShareLock blocker.

1. s2 RR BEGIN, read other → s1 REPACK → s2 count(t): 0 stock [R], 40001 with E.
2. s1 paused at the injection point → s2 RR snapshot → s1 commits → s2
   count(t) = 0: proves "before commit, not before start".
3. s3 locks t → s1 waits → s2 snapshot then count(t) queues → s3 commits → s2
   = 0 [R]; 40001 with E.
4. RC cursor + STABLE function: 40001 at first FETCH with E.
5. Negative control: s2 touches t first → s1 waits, correct counts, no error.
6. 1 and 3 for TRUNCATE and ALTER TABLE ... ALTER COLUMN TYPE.
7. VACUUM FULL instead: correct counts stock; shows the false positive if not
   allowlisted.
8. COPY t TO STDOUT after s1 commits: 40001 (ProcessUtility path).
9. GRANT on t after s2's snapshot: no error (historic relfilenode comparison).
10. Writer-side race: check passes → s2 snapshot → s1 commits → s2 reads 0:
    proves A and B are not race-free.

## Mailing-list sources

- [RFC: Making TRUNCATE more "MVCC-safe" (Raudsepp, 2012)](https://postgresql.org/message-id/CABRT9RBRMdsoz8KxgeHfb4LG-ev9u67-6DLqvoiibpkKhTLQfw%40mail.gmail.com)
- [Re: RFC (Noah Misch)](https://postgresql.org/message-id/20120210114205.GA23917%40tornado.leadboat.com)
- [Re: RFC (Robert Haas)](https://www.postgresql.org/message-id/CA%2BTgmobtjc4edN5MY6Z6jzQLp_xsB3gJYAJCB4sqKsg%2BazztJQ%40mail.gmail.com)
- [Re: TRUNCATE SERIALIZABLE and frozen COPY (Simon Riggs)](https://www.postgresql.org/message-id/CA%2BU5nMKX7D6NR%2BVwmkxaueu7Ud-m3y_sy9VkUNhtrgeA6eTt3w%40mail.gmail.com)
- snapshot-too-old removal: commit f691f5b80a8
