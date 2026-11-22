# Walk ledger: pg_repack_safe, whole repo at c773731 (branch record-based-guard)

Lens: is this publishable, maintainable, understandable for the wider PostgreSQL community?
Walk finished 2026-11-21 (8 chunks). Flat table first; the closing grouping is at the end of this file.

| # | Where | What | Severity | Confidence | Status |
|---|-------|------|----------|------------|--------|
| A | src/pg_repack_safe.c:107 | WAL resource manager used RM_EXPERIMENTAL_ID (128). Now 149, the ID reserved for pg_repack_safe on the PostgreSQL wiki (read from the live page 2026-11-21); full installcheck passes. | risk | high | fixed |
| B | src/pg_repack_safe.c:660-687, :1196, :1627-1628; README.md:102-103; doc/research/fix-design.md:576-597 | Records are never deleted, by a recorded decision (2026-11-16): the first cleanup design gave a silent wrong result on a standby (a row deleted by its own inserter is vacuumed there with no recovery conflict, heapam.c:8228-8239), so it was reverted and deferred with a TODO and a stated rule. Consequence today: each backend holds a full copy, every REPACK makes every backend rescan the whole table, about half the records name dropped TOAST relations (probed), and the only way to shrink the table is the full "Removing the extension" procedure. README says the table only grows but not what that costs or how to reset it. Cost not measured. | risk | high on mechanism, cost unmeasured | fixed in 9ba5cef (next-writer deletion, pg_repack_safe_prune_records, rewrite refusal while a deleted record exists, new removal procedure) |
| C | repo root; Makefile:23-25, :39-42; doc/development.md:10-16 | No CI and no PGXN META.json. The suite has only ever run on one machine (aarch64 Linux, gcc 13.3, a source build of 19rc2 with TAP enabled; `pg_config --version` and `uname -m` on 2026-11-21). It needs a TAP-enabled server build, IPC::Run, pg_stat_statements and pageinspect, and `make installcheck` installs a test module into the server's library directory. On a build without TAP, PGXS only prints "TAP tests not enabled" (Makefile.global:509), so 15 of 27 test files would be skipped silently. | risk (raised from smell) | high | CI added in 729f34c (.github/workflows/ci.yml), not yet run: nothing is pushed. PGXN META.json still missing. The silent skip of TAP tests on a build without TAP is unchanged |
| D | git; GitHub | The extension exists only on local branch record-based-guard (22 commits, not pushed; `git ls-remote --heads origin` shows only main at 0257aaf). The GitHub repo is private (`gh repo view`, 2026-11-21), while the PostgreSQL wiki row for resource manager 149 already links to it. | smell | high | open (33 commits ahead of origin/main, still unpushed) |
| E | pg_repack_safe.control:4; src/pg_repack_safe.c:379-433 | Probed 2026-11-21 on a preloaded temp instance: after ALTER EXTENSION pg_repack_safe SET SCHEMA ext, queries pass, REPACK (CONCURRENTLY) records into ext.pg_repack_safe_rewrites, and UPDATE/DELETE, DISABLE TRIGGER and DROP TABLE stay refused. Works; the remaining gap is that no permanent test pins it. | smell (test gap) | high | fixed in 7d39364 (SET SCHEMA now tested in sql/protection.sql) |
| F | src/pg_repack_safe.c (1966 lines, one file) | The whole library is one C file with six concerns (cache, reader, writer, WAL, protection, hooks); readability for outside contributors not yet judged. | smell | low | open (provisional) |
| G | doc/research/history.md:1, :198-214, :215-286; doc/research/fix-design.md:479-800; README.md:91-92 | The research notes are working papers, already on origin/main. fix-design.md is half implementation log (checkbox steps, dated developer rulings) and is what the README links as "the design and its evidence". history.md is titled "how the wrong results were accepted", has sections "Did the extension precedent cloud the judgement?" and "The 'AI slop' angle", quotes named community members, lists unverified sources, and ends with a stale "Draft README section" whose last line is "TODO: state the mechanism once chosen". Publishing these as-is sets the tone of the project's relationship with the people who wrote REPACK. | risk (raised from smell) | high | open |
| H | README.md:58-63, :135-173; specs/known_gaps.spec:69-73 | "Known gaps" mixes three real REPACK gaps (FK error class, never-loaded session, standby replay window) with stock behaviour outside the extension's scope (TRUNCATE, rewriting ALTER, REFRESH MATVIEW, VACUUM FULL on standbys); the first three known_gaps permutations test PostgreSQL, not the extension. Developer raised this in the walk. | smell | high | confirmed |
| I | src/pg_repack_safe.c:416-426, :501-508; sql/reader_check.sql:69-89 | Confirmed: with the record table renamed, moved or missing a trigger, even SELECT 1 is refused database-wide. Only the table's owner or a superuser can cause it; utility statements still run, so RENAME back and DROP EXTENSION fix it (tested reader_check.sql:74, :89). Documented at README.md:119-121. | smell | high | open (downgraded from risk; candidate for Accept) |
| J | pg_repack_safe.control:3; CHANGELOG.md:8; AGENTS.md:124-128 | Version 1.0.0 with everything under [Unreleased], no tag, and no upgrade-script story; deliberate for prerelease (AGENTS.md:127-128). The planned fixes P, V, Y and B all change the install script or on-disk behaviour, so they belong before the first tag. | smell | high | open |
| K | src/pg_repack_safe.c:797-801; README.md:31-51 | Probed 2026-11-21: from an old REPEATABLE READ snapshot, UPDATE and DELETE of a repacked table get the 40001 error (on stock they would silently change 0 rows). Correct and valuable, but the README promises only "a read" and no permanent test runs old-snapshot DML. | smell (doc + test gap) | high | open |
| L | src/pg_repack_safe.c:797-801; README.md:46-51 | Probed 2026-11-21: a plain INSERT ... VALUES into a repacked table from an old snapshot is refused with "cannot read table", although it reads nothing and stock handles it correctly. A second spurious error, not listed in the README (which names one), with a message that says "read". | smell | high | open |
| M | README.md (whole) | The README never shows the failure or the fix as SQL: no two-session example of stock returning 0 rows, and no example of the 40001 error with its DETAIL/HINT. The only place the full error text appears is doc/research/fix-design.md:260-263. | smell | high | open |
| N | expected/repack_concurrently_*.out; README:54-56 | "The reproducers that fail on stock PostgreSQL 19" cannot be run against stock from this repo: the expected files pin the guarded output and the stock output survives only in spec comments (and in git at 0257aaf). | smell | high | open |
| O | AGENTS.md:13 vs README.md:24 vs repack.sgml:324 | Three wordings of who is affected: upstream and AGENTS.md say snapshots taken "before repacking started"; the README and the code say before it *committed* (wider, and the one the check implements at src/pg_repack_safe.c:773). The README does not say it is deliberately wider than the upstream warning. | smell | medium | open |
| P | src/pg_repack_safe.c:1187, :509-517; README.md:193-199 | Probed 2026-11-21: CREATE INDEX and ADD PRIMARY KEY on the record table are accepted; after two more REPACKs a heap scan sees 8 records (max xid 674) while max(rewrite_xid) through the index returns 668 and an index scan 4 rows. The reader guard is unaffected (heap scan), but the README removal query uses max(rewrite_xid) and would under-report, so an operator could drop the extension while an old snapshot still needs a record. | risk (raised from smell) | high | fixed in 7d39364 |
| Q | src/pg_repack_safe.c:421-425, :505-508, :515-517 | The three "cannot find/use its record table" errors hint "drop and re-create the extension" without saying that this discards the records, or that the drop is refused without pg_repack_safe.allow_unprotected_drop while records exist. | smell | medium | open |
| R | src/pg_repack_safe.c:876-878, :818-821; README.md:95-100 | COPY of a partitioned table takes ShareUpdateExclusiveLock on it before checking whether any record, or even the extension, exists. Probed 2026-11-21 in a database without the extension: the COPY held ShareUpdateExclusiveLock on the parent. So preloading the library changes COPY locking (blocks ATTACH PARTITION, ANALYZE, a second COPY of the same parent) server-wide; the README describes the lock but not that it applies everywhere. | risk | high | fixed in 67cf120 |
| S | src/pg_repack_safe.c:875-878 | checkCopyTo takes its locks, including the ShareUpdateExclusiveLock, before COPY's own permission check runs in DoCopy, so a user with no rights on a partitioned table can briefly take that lock (released when the statement fails). Stock takes only AccessShareLock before its permission check. | smell | medium (read from code; lock is short-lived) | resolved for partitioned tables in 67cf120 |
| T | src/pg_repack_safe.c:1063-1073 | The pre-check's error DETAIL names another session's PID and transaction start time. pg_stat_activity hides those columns from users without pg_read_all_stats or membership in the session's role (pgstatfuncs.c:39, :331), and the pre-check setting is PGC_USERSET, so a table owner can learn them through the refusal. | smell | medium (read from code, not run as an unprivileged role) | open |
| U | src/pg_repack_safe.c:1737-1752 | isConcurrentRepack is true for any RepackStmt with the option, including CLUSTER (CONCURRENTLY) and VACUUM-FULL-shaped statements that ExecRepack rejects (repack.c:286-293); such a statement then reports the extension's refusal (preload, extension missing, pre-check) instead of upstream's "CONCURRENTLY option not supported". Fails either way. | smell | medium (read from code) | open (candidate for Accept) |
| V | pg_repack_safe--1.0.0.sql:28-32; src/pg_repack_safe.c:577-585; doc/installation.md:91-105 | Probed 2026-11-21 with two clusters, the extension on both, and CREATE PUBLICATION ... FOR ALL TABLES: the publication includes pg_repack_safe_rewrites, and a REPACK (CONCURRENTLY) on the publisher put its record (relid 16394, xid 3667) on the subscriber, where OID 16394 is an unrelated table. There, every SELECT and INSERT on that table failed with "unsafe-rewrite record with a transaction ID that has not been assigned" (next xid 684) until the subscriber's XID counter passed 3667; after that, new snapshots read it but a snapshot taken earlier got the 40001 error. The operator cannot DELETE the foreign record (trigger refuses). Fails closed (no wrong rows), but takes an unrelated table offline. Not mentioned in any doc. | bug (raised from risk) | high | fixed in ce66b91; the writer's record is no longer sent at all since 9e86441 (HEAP_INSERT_NO_LOGICAL), so the trigger is the backstop for the initial copy and other sources |
| W | README.md:66-73; doc/installation.md:38-50 | The custom WAL record means every later consumer of this WAL needs the library installed: PITR restores from the archive, delayed replicas, base-backup clones, and crash recovery of the primary itself, for as long as retained WAL holds such a record, even after the extension is dropped. Docs say "anything else that replays the WAL" but never name backups/PITR, and do not say that platforms where the library cannot be preloaded (managed services) cannot use the extension. | smell (doc) | high | open |
| X | t/002_standby_record_removal.pl:1-22 | This TAP test does not load the extension: it probes stock PostgreSQL on a plain table to establish the rule a future record-pruning feature would rely on. It is design evidence for unbuilt work (B) living in the shipped test suite. | smell | high | resolved by 9ba5cef: t/002 is now the evidence for shipped behaviour |
| Y | src/pg_repack_safe.c:509-517, :1701-1712; sql/protection.sql | Probed 2026-11-21: ALTER TABLE pg_repack_safe_rewrites ADD COLUMN is accepted, and from then on every query in the database fails ("cannot use its record table"), including catalog queries and the README's old-snapshot check. DROP COLUMN of the added column, RENAME COLUMN and VACUUM FULL do not restore service. The only way back was DROP EXTENSION with allow_unprotected_drop, which discards the records, and the operator cannot run the removal procedure's snapshot check in that database first. Owner or superuser only; fails closed. | risk | high (probed) | fixed in 7d39364 |
| Z | src/pg_repack_safe.c:297-312; README.md:101-121 | Protection identifies the trigger functions by name and schema only, so an owner can CREATE OR REPLACE one with a no-op body and then delete records. The protection is against accidents, not against the table's owner (who also has the override setting); the README says "protected" without that qualifier. | smell | medium (read from code) | open (candidate for Accept) |
| AA | src/pg_repack_safe.c:1832-1862; doc/installation.md:107-113 | Statements this library refuses in its ProcessUtility hook (REPACK (CONCURRENTLY) without preload or extension, the pre-check, a refused COPY TO) never reach hooks of libraries loaded before it, so pg_stat_statements or an audit extension loaded earlier does not see the attempt. Deliberate and documented in the installation guide; depends on load order, which the guide does not advise on. | smell | medium (read from code; t/004 tests successful statements in both orders, not refused ones) | open (candidate for Accept) |
| AB | doc/research/fix-design.md:791-797; README.md:86-90 | The only performance measurement (about 1 microsecond per query, at most about 2%, select-only pgbench, record table near-empty) lives in the research note. The README says the check "costs about as much as a hash lookup" with no number, and nothing measures the cost with many records or right after a REPACK invalidates every backend's cache (B). | smell | high | open |
| AC | Makefile:28-30; test/modules/oat_observer/Makefile | The build uses -Wall -Wextra -Werror for everyone, not only for development. A user or packager with a newer compiler or different PostgreSQL build flags gets a failed build from any new warning; the design note records this already happening once with release headers (fix-design.md:797-800). | risk | high | fixed in 5f16048 (-Werror only with make WERROR=1) |
| AD | AGENTS.md:11, :51, :87, :101, :124; pg_repack_safe.code-workspace:8-41; project_template.md:8-30; .claude/skills/ | Tracked files point at one developer's machine and private material: /opt/pg19 paths, ~/dev/datastone/pg19/env.sh, a style "constitution" at ~/dev/pg-extension-template that is not in the repo (so AGENTS.md rule 7 cannot be followed by an outside contributor), and a generic project template naming files this repo does not have (code_review_checklist.md, doc/operator-reference.md, .specify/). | smell | high | open |
| AE | README.md:1-15; pg_repack_safe.control:2 | The name reads as "a safe pg_repack". pg_repack is a long-established third-party extension, and this project has nothing to do with it (history.md notes core REPACK descends from pg_squeeze, not pg_repack). Neither the README's opening nor the control comment says so. The name is already reserved on the PostgreSQL wiki. | smell | high | open |
| AF | AGENTS.md:124; doc/research/history.md:217 | Everything was built and tested against 19rc2. Today is 2026-11-21 and the history note says GA was expected 2026-11-19 (unverified there); no run against a GA build is recorded. Source line citations in docs and tests (repack.c:3584, heapam.c:8228-8239, ...) are pinned to the rc2 tree. | risk | medium (GA status not checked in this walk) | tested on 19.0, 2026-11-22: /opt/pg19 rebuilt from the REL_19_0 tag (it was rc2 plus 42 commits, five short of the release; the one code change between them is in VACUUM's row estimates), extension rebuilt from clean with WERROR=1, full installcheck passes (185 TAP tests, 7 regress, 8 isolation). Source line citations not re-checked one by one |
| AG | README.md:19-63; doc/research/history.md:10-14, :262-264 | The README does not say what the research already established and a user needs for planning: an MVCC-safe REPACK patch is under review for PostgreSQL 20 (so this extension may be a PG19-only bridge), and the error-instead-of-empty behaviour was proposed and declined upstream. | smell | medium (history.md's own citations not re-read in this walk) | open |
| AH | /opt/pg19/src/src/backend/replication/logical/worker.c:3803; src/pg_repack_safe.c OAT_TRUNCATE case | Found while building V (agent probe, not in the repo): a publisher's TRUNCATE of its record table, replicated through FOR ALL TABLES, emptied a subscriber's record table although a local TRUNCATE there is refused; apply calls ExecuteTruncateGuts directly and never reaches OAT_TRUNCATE. The subscriber's old snapshots lose their protection: a wrong-results path. Excluding the table from the publication (now documented for V) closes it operationally; not fixed, not pinned by a test, not in README "Known gaps". Fixed 2026-11-22: the refusal moved from the OAT_TRUNCATE case into the BEFORE TRUNCATE trigger (pg_repack_safe_records_protected); t/016_publication.pl reproduced the emptied table before the fix and pins the refusal and recovery. | bug | high (reproduced in the main session) | fixed in 76f2b1f |
| AI | pg_repack_safe--1.0.0.sql (trigger pg_repack_safe_rewrites_changed); specs/reader_cache.spec | After V, the AFTER statement trigger's INSERT/UPDATE/DELETE events can fire only for statements that change no row, and TRUNCATE sends its own relcache invalidation (read from source by the V agent, not tested); the trigger and its definition check may be dead weight, and the replica-role permutation of reader_cache.spec is now redundant. Decide with B. | smell | medium | fixed in 91fb05b (trigger removed; specs/reader_cache.spec shows a cache following a TRUNCATE without it). The replica-role permutation of reader_cache.spec is still redundant |
| AJ | src/pg_repack_safe.c insertRecord; sql/protection.sql | Found while building P/Y: table constraints (ADD CHECK, ADD FOREIGN KEY) on the record table are accepted and are not enforced for the writer's direct inserts. No wrong query results. | smell | medium (read from code) | accepted: constraints stay allowed; bfeb3f6 pins that ON DELETE CASCADE cannot delete records |
| AK | Makefile install-test-modules; test/modules/test_record | After V, `make installcheck` installs test_record.so into the server's library directory; a superuser can use it to write arbitrary records. The regress and isolation suites now depend on it too. doc/installation.md warns against running installcheck on a production installation. | smell | high | accepted by the developer 2026-11-22: the warning in doc/installation.md is enough |
| AL | t/007_standby_reader.pl:85-103; doc/research/fix-design.md step 6 (probe 2026-11-22) | Found by B's step 0 probe: the test's "old snapshot gets the error after the REPACK and a VACUUM" assertion runs before the VACUUM is replayed (wait_for_catchup targets the primary's write position, which was the start of the VACUUM's prune record). Once replayed, that record (snapshotConflictHorizon = newest record's XID) terminates the idle standby reader. The test claims coverage it does not have, and README/docs do not say that a vacuum of the record table already ends old standby snapshots. | bug (test) | high (waldump plus diagnostic run) | fixed in 9ba5cef |
| AM | src/pg_repack_safe.c deleteOlderRecords | A record deleted by another transaction that commits between the cleaner's snapshot and its heap_delete (TM_Deleted/TM_Updated) is not exercised by any test; the code leaves the row alone. The local build has no injection points; the weekly CI job builds with them. | smell (test gap) | high | open |
| AN | .github/workflows/ci.yml | The workflow has never run. Unverified: the directory-ownership step, the source build's dependency list, and whether the arm64 runner is available to a private repository. | risk | high | open |
| AO | README.md Known gaps; doc/research/fix-design.md step 6 | On a standby without hot_standby_feedback, replaying a VACUUM of the record table ends every snapshot in that database older than the latest REPACK (CONCURRENTLY). Accepted by ruling 3 and documented; the README's "about 50 deleted records" for autovacuum is from the default thresholds, not from a run. | smell | medium | accepted, one figure unverified |

## Planned fixes (after the walk; requested by the developer)

### P: refuse index creation on the record table

**Done in 7d39364.**

Decided 2026-11-21, during chunk 2. Not started.

- **Test first** (`sql/protection.sql` + `expected/protection.out`): with the extension created, each of these on
  `pg_repack_safe_rewrites` must fail with the new error, and leave no index behind:
  `CREATE INDEX`, `CREATE UNIQUE INDEX`, `CREATE INDEX CONCURRENTLY`, `ALTER TABLE ... ADD PRIMARY KEY`,
  `ALTER TABLE ... ADD UNIQUE`. Control: `CREATE INDEX` on an ordinary table still works.
  Also check the refusal holds after `ALTER EXTENSION ... SET SCHEMA` (closes E's test gap in the same file).
- **Mechanism:** new `OAT_POST_CREATE` case in `objectAccessHook` (`src/pg_repack_safe.c:1688`) for
  `classId == RelationRelationId && subId == 0`. Every index creation route reaches `index_create`, which fires the
  hook at `/opt/pg19/src/src/backend/catalog/index.c:1233-1234`.
- **Catch:** the hook fires before `CommandCounterIncrement` (`index.c:1240`), so the new `pg_index` row is not visible
  to a syscache lookup. Find the index's table with a `SnapshotSelf` scan of `pg_index` on `IndexRelidIndexId`, as
  `selfVisibleRelfilenode` does for `pg_class` (`src/pg_repack_safe.c:1233`). No row found means the new relation is
  not an index: return.
- **Refusal:** if `indrelid` is the record table (`isRecordTable`), `ereport(ERROR)`; always refused, no override GUC
  (same class as UPDATE/DELETE of records). State the invariant in the function header: the writer inserts with
  `simple_heap_insert` (`:1187`), which maintains no index, so an index would miss every later record.
- **Not doing:** no "table has an index" test in `checkRecordTableDefinition`. A stale index cannot make the reader
  guard wrong (it heap-scans), so a database-wide refusal would be out of proportion.
- **Open point to settle when fixing:** `REINDEX` and `VACUUM FULL`/`REPACK` of the record table create no index today
  (it has none, and no TOAST table); confirm the new case does not fire for them, and that `pg_upgrade`
  (`t/003_pg_upgrade.pl`) still passes.
- **Docs:** README "How it works" protection list, CHANGELOG `[Unreleased]`, and the hook's census comment.
- **Residual gap:** a backend that never loaded the library can still create the index (already listed in the README
  as the never-loaded-session gap).
- **Check:** new protection cases fail before the change and pass after; full
  `. ~/dev/datastone/pg19/env.sh && make && make install && make installcheck` passes.

### V: let only the writer insert records

**Done in ce66b91; see also 76f2b1f (TRUNCATE) and 9e86441 (record not sent).**

Decided 2026-11-21, during chunk 5. Not started. Direction proposed in the walk and accepted by the developer; the
design below is read from two code sites, not yet built or probed.

- **Problem (probed):** `CREATE PUBLICATION ... FOR ALL TABLES` includes `pg_repack_safe_rewrites`; a publisher's
  record lands on a subscriber that has the extension, names whichever table has that OID there, and takes it
  offline ("transaction ID that has not been assigned") until the subscriber's XID counter passes the record's;
  the operator cannot delete it.
- **Test first:**
  - TAP (new `t/016_publication.pl`, two clusters, extension on both, `FOR ALL TABLES`, subscriber table created so
    its OID equals the publisher's repacked table): today the subscriber's table errors; after the fix it must stay
    readable and writable, and the subscriber's record table must stay empty. Assert what the apply worker reports.
  - regress (`sql/protection.sql`): `INSERT`, `COPY ... FROM`, `INSERT ... SELECT`, `MERGE` into the record table are
    refused, also under `session_replication_role = replica`; `REPACK (CONCURRENTLY)` still records.
- **Mechanism:** a row-level `BEFORE INSERT` trigger on the record table, `ENABLE ALWAYS`, calling a C function that
  always raises. The writer inserts with `simple_heap_insert` (`src/pg_repack_safe.c:1187`), which fires no
  triggers, so it is unaffected. The apply worker fires row-level insert triggers
  (`/opt/pg19/src/src/backend/executor/execReplication.c:905`); confirm the same for the initial tablesync COPY.
- **Also needed:** `checkRecordTableDefinition` (`:480-518`) and `protectRecordTrigger`/`isRecordTriggerFunction`
  must know the third trigger, so a database without it refuses, as for the other two; `pg_upgrade`
  (`t/003_pg_upgrade.pl`) must still pass (binary upgrade copies files and fires no triggers).
- **Tests to rework:** those that insert records by hand: `sql/reader_check.sql:37-54, :101`,
  `specs/reader_cache.spec` step `o_record`, and any in `t/` (grep `INSERT INTO pg_repack_safe_rewrites`). The
  `pg_repack_safe_records_changed` AFTER trigger's INSERT event then has no caller left; decide whether it stays.
- **Open points to settle when fixing:**
  - What the subscriber should do next: the apply worker will error and retry on that table. Decide the documented
    remedy (exclude the table from the publication on the publisher; PG19 syntax to be checked) and whether a
    `NOTICE`/hint in the error should say so.
  - Whether to also keep the publisher from publishing the table at all (no extension-side opt-out is known; check
    PG19's publication options before claiming so).
  - Existing foreign records in a database upgraded to the fixed version: none can exist before a release
    (prerelease, install script edited in place).
- **Docs:** README protection list; `doc/installation.md` new "Logical replication" section next to "Dump and
  Restore"; CHANGELOG `[Unreleased]`.
- **Check:** new tests fail before and pass after; full
  `. ~/dev/datastone/pg19/env.sh && make && make install && make installcheck` passes.

### B: bounded records, "the next writer deletes, VACUUM decides when"

**Done in 9ba5cef. The text below is the plan as written before the work.**


Discussed 2026-11-21 after chunk 6; promoted to a planned fix by the developer the same day. Not started. Largest of the planned fixes: it changes the writer, the protection rules and the standby story, so it gets its own step plan (AGENTS.md principle 6) before any code.

- **Idea:** at its swap, after inserting its own record, each REPACK (CONCURRENTLY) deletes every older record
  (rewrite_xid < its own full XID, inserter no longer in progress), with a direct heap delete that fires no trigger.
  Nothing decides "is this record still needed": the deleted row version stays physically present until VACUUM may
  remove it, and the reader already scans with SnapshotAny (`src/pg_repack_safe.c:661`).
- **Why it is safe (the design note's rule, `doc/research/fix-design.md:369-380`):** deleter D is not the inserter and
  D follows R. Primary: a snapshot that does not see R has xmin <= R < D, so VACUUM cannot remove the row while it
  lives. Standby: the removal's conflict horizon is D (`heapam.c:8234-8240`, xmax != xmin), so replay cancels any
  standby snapshot with xmin <= D instead of letting it read. `t/002_standby_record_removal.pl` shows both on a plain
  table. Verified in the walk: the heapam.c lines; not re-run with the real table.
- **Avoids the reverted design's bug:** that one deleted the record in its inserting transaction (xmax == xmin), which
  the horizon ignores.
- **Must be settled when building:**
  - XID order: the cleaner's XID must follow each record it deletes; a concurrent REPACK of another table can hold a
    higher XID and commit first, so compare full XIDs and skip newer or in-progress records.
  - Two REPACKs cleaning at once: `simple_heap_delete` raises "tuple concurrently deleted" (`heapam.c:3249-3250`),
    which would fail a finished REPACK. Serialize cleaners or tolerate the result.
  - The cleanup runs at the swap, under AccessExclusiveLock on the user table: it cannot run earlier, because
    REPACK (CONCURRENTLY) must not have an XID before decoding setup (`repack.c:312-324` comment). Keep it short.
  - Autovacuum settings for the record table so dead records are removed promptly.
  - A SQL function for databases that stop repacking (the last records otherwise stay), which is the TODO at
    `src/pg_repack_safe.c:1627`.
  - Cost on standbys without hot_standby_feedback: queries with snapshots older than the latest REPACK are cancelled
    when the removal replays (ordinary recovery-conflict behaviour).
- **Considered and not preferred:** storing the XID as a security label on the table (one per live relation, dropped
  with it, never published) - pg_dump emits security labels, so dumps would carry foreign XIDs and fail to restore
  without the library. One database-wide high-water XID - spurious errors on every table.
- **Test first, in this order:**
  1. TAP, standby without hot_standby_feedback, library on both nodes, the real record table: a standby snapshot
     from before REPACK 1 must still get an error (the guard's 40001 or a recovery conflict), never rows, after
     REPACK 2 deleted record 1 and the primary vacuumed the record table. This is the test the reverted design failed
     (`doc/research/fix-design.md:586-588`).
  2. Isolation: an old primary snapshot still errors after a later REPACK deleted its record and VACUUM ran.
  3. Isolation: two REPACKs of different tables finishing together both succeed.
  4. Regress: after N repacks of one table plus VACUUM, the record table holds a bounded number of row versions;
     no record names a dropped TOAST relation.
  5. The SQL prune function: refused or a no-op where the rule does not hold; works in a database that stopped
     repacking.
- **Docs:** README "How it works" (records no longer only grow), "Known gaps"/standby notes (query cancellation on
  standbys without feedback), CHANGELOG; update `fix-design.md` step 6 with the outcome; `t/002` becomes evidence
  for shipped behaviour (closes X).
- **Interaction with the other planned fixes:** V's insert trigger and this cleanup both bypass triggers through
  direct heap calls; P's index refusal keeps the direct insert/delete from leaving a stale index.

#### B: step plan from the read-only planning pass (2026-11-21)

Nothing was built or run for this plan; every PostgreSQL claim is from reading /opt/pg19/src and anything a test must
confirm is under "Not verified". Line numbers in src/pg_repack_safe.c are as of 67cf120. Awaiting developer rulings on
the decisions; no code until then, and not before V is committed (the tests use V's `test_insert_record`).

**Developer rulings (2026-11-22)**

- Decision 1: option B. A rewrite of the record table (VACUUM FULL, CLUSTER, plain REPACK) is refused only while
  its old storage holds a deleted row version, with a hint to run plain VACUUM on the record table first. Option C
  (a WAL record whose replay raises the conflict) is deferred, tied to the user-table standby gap if that is ever
  taken on.
- Decision 2 (removal procedure: stop repacking, prune, VACUUM to zero blocks, DROP EXTENSION): agreed.
- Decision 3 (standby cancellations accepted and documented): agreed. Step 0 showed a plain VACUUM of the record
  table already ends old standby snapshots today (fix-design.md step 6, probe 2026-11-22; ledger AL).
- Decision 7 (leave autovacuum defaults): agreed.
- Decisions 4-6, 8, 9: the planner's recommendations are followed.
- Step 0b: no injection points in the build; the TM_Deleted path needs another technique or stays unverified.

**Decisions needed (planner's recommendation in brackets)**

1. Rewrites of the record table. `protectRecordTableFromRewrite` lets VACUUM FULL, CLUSTER and plain REPACK rewrite
   the record table because "those keep every record" (fix-design.md:703-707). Once records are deleted that is false:
   the rewrite drops versions that are DEAD by the primary's horizon (heapam_handler.c:789-793, :840) and logs no
   conflict horizon (pinned for user tables by t/012). A standby snapshot from before R would find no record and read
   the table as empty; the table would also be at zero blocks, so DROP EXTENSION would pass without the override.
   Options: (A) refuse every rewrite of the record table (database-wide VACUUM FULL / REPACK then fails at it);
   (B) refuse only when the old storage holds a version with a valid xmax; (C) allow it and add a second WAL record
   type under rmgr 149 whose redo calls ResolveRecoveryConflictWithSnapshot (standby.c:470). [A; reverses a recorded
   decision; removes inMvccSafeRewriter. Must land before the cleaner.]
2. Removal procedure. README's `max(rewrite_xid)` reads live rows only; after a prune there may be no live row while
   deleted versions are still needed, so the query lists nobody. [Replace with: stop repacking, call the prune
   function, VACUUM the record table until zero blocks, DROP EXTENSION without the override; "any block counts" in
   refuseUnprotectedRemoval becomes the whole check; no per-standby query.]
3. Standby cost. Without hot_standby_feedback, replaying the removal ends every transaction in that database with
   xmin <= D (the cleaning REPACK's XID), whether or not it would read a repacked table: active query gets 40001
   "conflict with recovery", idle-in-transaction is terminated FATAL (postgres.c:3266-3305). [Accept and document.]
4. Keep "D follows R" although only "deleter is not the inserter and the inserter committed" is necessary; compare
   in 64 bits on the column, plus a tuple-header check. [Keep.]
5. Concurrent cleaners: `heap_delete(..., wait = false)`, treat every result but TM_Ok as "leave it"; no lock (a
   serializing lock would collide with autovacuum while the user table is under AccessExclusiveLock). [Tolerate.]
6. Which records: every committed record with rewrite_xid below the cleaner's full XID, any relid. [All older.]
7. Autovacuum reloptions: defaults already bound the table (50 + 0.2 x reltuples dead rows, autovacuum.c:3311); a
   threshold of 0 would turn every REPACK into a standby cancellation within a naptime. [Do not set; the walk's
   earlier note leaned the other way.]
8. Prune function: `pg_repack_safe_prune_records() RETURNS bigint`, revoked from PUBLIC, no superuser check.
9. t/002 stays a stock probe and gains two cases; real-table tests go to t/007 and a new t/017.
10. Probe first: by pruneheap.c:1274-1276 and :1960-1962 a plain VACUUM that marks the record table's page
    all-visible logs a conflict horizon equal to the newest live xmin, which should already terminate t/007's idle
    standby reader today, yet t/007 passes. Not reconciled by reading. Decides what t/007/t/017 may assert and what
    "compared with today" means in the docs.

**Correctness argument (compact)**

- Reader: S "does not see R" means XidInMVCCSnapshot(R, S), so S.xmin <= R (snapmgr.c:1915); the backend advertises
  MyProc->xmin <= S.xmin (procarray.c:2361). The reader uses the column value and loads every version with
  SnapshotAny.
- Primary: a row deleted by committed D is RECENTLY_DEAD with dead_after = D (heapam_visibility.c:1326-1330) and
  DEAD only once D precedes the horizon (heapam_visibility.c:1125; pruneheap.c:1475-1477, :1488), which is at most
  S.xmin <= R < D (procarray.c:1797-1803). On-access pruning uses the same test (pruneheap.c:302-304) and never runs
  in recovery (:285).
- Standby: removal logs snapshotConflictHorizon >= D (heapam.c:8214-8241, xmax when xmin committed and xmax != xmin);
  replay collects backends of that database whose xmin does not follow it (heapam_xlog.c:125; procarray.c:3426-3427)
  and waits for them to end (standby.c:384), signalling after max_standby_streaming_delay. Reader's xmin <= R < D.
- Frozen xmin: horizon still D (htup_details.h:361-364, :374-377). Deleter aborts: row is LIVE again
  (heapam_visibility.c:1307-1314). Hint bits: not relied on (heapam.c:8236-8237 falls back to clog).
- Corrections to the walk's summary: it omitted record-table rewrites (decision 1); "cancels" means replay waits,
  then ERROR or FATAL; "D follows R" is sufficient, not necessary; VACUUM horizons skip PROC_IN_VACUUM backends
  (procarray.c:1770), no reader found under that flag but not censused.

**Mechanics**

- XID order: REPACK (CONCURRENTLY) gets its XID after decoding setup (repack_worker.c:238; repack.c:3892-3896,
  :1165-1191), long before the swap, so a shorter REPACK of another table can commit a higher-XID record first: skip
  records at or above own XID and in-progress ones. Newest record per relation suffices for the reader.
- Scan with a registered GetLatestSnapshot() so TM_Invisible (which raises, heapam.c:2840-2846) cannot occur; pass
  TABLE_DELETE_NO_LOGICAL (tableam.h:290) so the delete is never decoded for a publication.
- Runs in noteRelationAltered after both insertRecord calls, inside the `inUnsafeRewriter() && oldBlocks > 0` block
  (keeps decision 2's invariant that cleanup happens only in a transaction that also inserts a record). Cannot run at
  statement start (heap_delete assigns an XID, heapam.c:2767). CHECK_FOR_INTERRUPTS in the loop.
- Reader: no change; no invalidation needed for a delete; a cache holding a vacuumed record is harmless.
- pg_upgrade copies deleted versions intact; nothing is dumped.

**Steps (each: failing test, change, check)**

0. Probes, no code, results into fix-design.md: (a) pg_waldump of t/007's VACUUM - does its prune record carry a
   conflict horizon (decision 10); (b) does the PG19 build have injection points.
1. t/002: two more stock cases (row deleted by its inserter then VACUUM: standby reader not cancelled; idle reader
   with a row deleted by another transaction: terminated). Test and header only.
2. Record-table rewrite refusal (decision 1): sql/protection.sql:68-73 expects VACUUM FULL / REPACK of the record
   table refused (fails today); change protectRecordTableFromRewrite; README bullet.
3. Delete routine and prune function (ahead of the walk's order: REPACK (CONCURRENTLY) cannot run in a transaction
   block, so only a SQL-callable deleter can be held open for concurrency tests). Tests failing first with "function
   does not exist": new t/017_standby_record_cleanup.pl (standby without feedback, real table, active and idle old
   snapshots; after REPACK and prune replay a third old snapshot gets 40001; after the primary's VACUUM no reader
   ever returns a count, pageinspect shows the version gone on both nodes, a new snapshot reads 5 rows); new
   specs/record_prune.spec; new sql/records_prune.sql. Change: deleteOlderRecords(Relation, FullTransactionId) plus
   the SQL function and install-script entry. One-off mutation probe, logged: remove the own-xmin and R<D filters,
   t/017 must then show `after|0`; revert.
4. Wire the cleaner into the swap: rework t/007's last block (REPACK 3 is the deleter; "error or terminated, never
   rows"); spec permutations (old primary snapshot, prune held open while a REPACK swaps, two REPACKs together);
   sql/writer.sql (after N repacks and VACUUM at most the newest records are live, none names a dropped TOAST
   relation, the ANALYZE variant also cleans). Change: one call in noteRelationAltered.
5. Removal procedure (decision 2): t/017 gains prune, VACUUM, zero blocks, DROP EXTENSION without the override;
   sql/doc_examples.sql changes to the new procedure; README and the hints in refuseUnprotectedRemoval.
6. pg_upgrade: t/003 carries a deleted record version across; queries work and VACUUM removes it.
7. Docs and comments: README "How it works" and "Known gaps", doc/installation.md, CHANGELOG, fix-design.md section
   6c and step 6, remove the TODO, fix the "never deleted" comments. Full installcheck.

**Would abandon the design if:** a path physically removes a deleted version without a conflict horizon and cannot
be refused; a standby session state survives conflict resolution; a reader's snapshot xmin is not in its PGPROC; the
standby cancellations are unacceptable to users who cannot enable feedback; or step 3's mutation probe does not fail.

**Not verified:** nothing was run; decision 10; that nothing in cluster_rel assigns an XID before the decoding worker
starts; that GetLatestSnapshot is legal at the swap; the TM_Deleted path (needs an injection point); whether pruning
of the pg_class rows a REPACK replaces already cancels the same standby snapshots today; autovacuum actually firing
on the record table at defaults; that a database-wide VACUUM FULL aborts at the refused table rather than skipping it.

### Y: refuse column changes to the record table

**Done in 7d39364.**

Decided 2026-11-21, after the chunk 6 probe. Not started. The hook events below are from memory of the PG source
and must be confirmed with a `file:line` each before coding.

- **Problem (probed):** `ALTER TABLE pg_repack_safe_rewrites ADD COLUMN` is accepted, then every query in the database
  is refused; `DROP COLUMN` does not undo it; the only way back discards the records.
- **Invariant to enforce:** the record table's columns are fixed. Any column-level create, drop or alter of the
  record table is refused at its source, so `checkRecordTableDefinition` (`src/pg_repack_safe.c:480-518`) stays a
  backstop for backends without the library and is never reached by a backend with it.
- **Test first** (`sql/protection.sql`): each must fail with the new error and leave queries working:
  `ADD COLUMN`, `DROP COLUMN relid`, `ALTER COLUMN relid TYPE regclass` (binary-coercible, so no rewrite and not
  caught today), `ALTER COLUMN ... SET DEFAULT`, `RENAME COLUMN` (harmless, but refusing every column change keeps the
  rule one sentence; decide when fixing). Also refused with `allow_unprotected_drop = on`. Controls: `CREATE EXTENSION`,
  `DROP EXTENSION` with the override, `pg_upgrade` (`t/003_pg_upgrade.pl`) and `ALTER EXTENSION ... SET SCHEMA` all
  still work.
- **Mechanism:** in `objectAccessHook` (`src/pg_repack_safe.c:1688`), for `classId == RelationRelationId`,
  `subId != 0` and `isRecordTable(objectId)`: refuse `OAT_POST_CREATE` (new column), `OAT_DROP` (dropped column;
  `protectRecordsFromDrop` at `:1519` looks only at `subId == 0` today) and `OAT_POST_ALTER` (altered column). To
  confirm in the source: that ADD COLUMN, DROP COLUMN and ALTER COLUMN TYPE each invoke the hook with the column
  number as `subId`, and that dropping the whole table or extension does not invoke it per column.
- **Shares a code path with P:** both add cases to the same hook for the same reason; do them together.
- **Docs:** README protection list; CHANGELOG `[Unreleased]`.
- **Check:** new protection cases fail before and pass after; full
  `. ~/dev/datastone/pg19/env.sh && make && make install && make installcheck` passes.


## Design candidates (discussed, not decided)

### R: drop the partition lock by running partitioned COPY as a query

**Done in 67cf120 as option 1.**

Diagnosed 2026-11-21 after the walk. Chosen by the developer and implemented the same day (uncommitted): `copyPartitionedAsQuery` in src/pg_repack_safe.c, sql/copy_partitioned.sql, specs/copy_attach_race.spec. New item found while building P/Y, not yet in the table: table constraints (ADD CHECK) on the record table are accepted and not enforced for the writer's direct inserts.

- **Why the lock exists:** `checkCopyTo` lists and checks the partitions, then stock `BeginCopyTo` lists them again
  (`/opt/pg19/src/src/backend/commands/copyto.c:863`) and scans that second list. ShareUpdateExclusiveLock on every
  partitioned table in the tree keeps the two lists equal (`src/pg_repack_safe.c:816-843`).
- **Why it cannot just be skipped when there are no records (or no extension):** the hazard is a record written
  after the check. Check at T1 (nothing to refuse) -> REPACK (CONCURRENTLY) of a standalone table commits ->
  ATTACH PARTITION (allowed without the lock) -> the COPY's second listing, delayed by a lock wait on some
  partition, includes it and scans it with the old snapshot: rows missing. Contrived, but real, so by AGENTS.md
  principle 1 the skip is not available.
- **Candidate:** for a partitioned table only, rewrite `COPY p [(cols)] TO` into `COPY (SELECT cols FROM p) TO`
  in the hook. Stock already does this rewrite itself for row-level security (`copy.c:244-345`). The query form
  plans once, locks what it plans, scans only that, and is checked by the existing ExecutorStart path
  (`t/008_copy_guard.pl` already covers COPY (query)). Removes `listStablePartitions`, the lock, the unlock loop
  at `:1877-1878`, and flag S for partitioned tables.
- **To verify before choosing:** output identical to stock's relation path - explicit column list that skips
  dropped and generated columns as `copy.c:1076-1087` does (stock's own RLS rewrite uses `*`), partitions with a
  different column order, binary format; behaviour differences to document (a foreign-table partition is read
  instead of raising `copyto.c:870-878`'s error); throughput against the relation path; that pg_dump does not use
  COPY of a partitioned root (believed, not checked).
- **Probed 2026-11-21 on stock behaviour (PG19rc2, library preloaded, P/Y build):**
  - Output: `COPY p TO` and `COPY (SELECT <non-dropped, non-generated columns> FROM p) TO` were byte-identical in
    text, csv and binary, over a parent with a dropped column, a stored and a virtual generated column, a partition
    with a different physical column order and its own dropped column, and a sub-partitioned level. `SELECT *` was
    NOT identical (it adds the generated columns). The column-list form was identical too.
  - A rewrite must keep one error: `COPY p (id, g) TO` with a generated column raises "column "g" is a generated
    column" in the relation form, while the query form outputs it.
  - Locks: relation form today holds `p:AccessShareLock, p:ShareUpdateExclusiveLock`; query form holds only
    AccessShareLock on the parent and partitions.
  - Foreign-table partition: relation form errors ("cannot copy from foreign table"), query form returned its row.
  - Throughput, 4M rows in 8 partitions to /dev/null, 3 runs: text 2357-2403 ms relation vs 2468-2488 ms query
    (about 4-5% slower); binary 1567-1568 ms vs 1668-1683 ms (about 6-7% slower). Debug (-O0) server build.
  - pg_dump sends `COPY public.<leaf> (...) TO stdout` per leaf partition, also with --load-via-partition-root; it
    never copies the partitioned root.
  - Logical replication's initial sync DOES use the relation form on a partitioned root against a PG19 publisher
    (`/opt/pg19/src/src/backend/replication/logical/tablesync.c:1109-1114`), so it is the main real user of this
    path: it takes the lock today and would take the slowdown under the rewrite.
- **Not probed:** the rewrite itself (not built), `COPY ... TO` options beyond FORMAT, and an optimised build.
- **Fallback if rejected:** keep the lock and state its server-wide reach in README "How it works" and
  doc/installation.md.

## Closing grouping (2026-11-21)

**Fix before shipping**
- V (bug): record table is published by FOR ALL TABLES; foreign record takes an unrelated subscriber table offline. Planned.
- B (risk): records only grow; every REPACK makes every backend rescan all of them. Planned (largest; needs its own step plan).
- Y (risk): ADD COLUMN on the record table refuses every query, no non-destructive way back. Planned, with P.
- P (risk): an index on the record table goes stale and misleads the removal query. Planned, with Y.
- R + S (risk/smell): COPY of a partitioned table takes ShareUpdateExclusiveLock server-wide, before any record or permission check. Not planned yet.
- AC (risk): -Werror for every builder.
- C + AF (risk): no CI, one platform, rc2 only; TAP tests skipped silently on builds without TAP.
- G (risk): decide what of doc/research ships publicly before the repo is made public.

**Document or surface**
- W: WAL record's reach (PITR, clones, managed services). M + N: no runnable demo of the stock bug or the error.
- H: split real REPACK gaps from out-of-scope stock behaviour. K: UPDATE/DELETE protection is unclaimed and untested.
- L: plain INSERT refused (second spurious error). AB: performance numbers. AE: not the pg_repack extension.
- AG: PG20 outlook and upstream history. Q: error hints. O: "before commit" vs upstream's "before start".
- AA: load-order effect of refusals. Z: protection is against accidents, not the owner. T: pre-check discloses PID/xact start.
- AD: machine-specific and private references in tracked files. D + J: push, make public, tag after the planned fixes.

**Accept** (reason verified in the walk)
- I: damaged record table refuses all queries - documented README.md:119-121, tested sql/reader_check.sql:69-89, recoverable by utility statements (:74, :89).
- U: CLUSTER (CONCURRENTLY) gets this extension's refusal first - fails either way, repack.c:286-293.
- F: one 1,970-line C file - sectioned (src/pg_repack_safe.c:232, :521, :899, :1384, :1669, :1900), each function headed by its invariant.
- E: ALTER EXTENSION SET SCHEMA works (probed); its test is folded into P's plan.
- X: t/002 tests stock PostgreSQL - becomes evidence for shipped behaviour once B is built.
- Foreign-key check returns 23503 not 40001 - README.md:149-155, expected/known_gaps.out:76; no wrong rows.
- Replay-start window on a standby without the library - README.md:164-166, doc/research/fix-design.md:876-890.
- A: fixed in 053b4e4.

**Out-of-scope debt this surfaced** (stock PostgreSQL, not this extension)
- REFRESH MATERIALIZED VIEW shows an old snapshot the new contents (expected/known_gaps.out:46). Whether upstream documents it: unverified (specs/known_gaps.spec:19-20 says not). Track: pgsql-hackers / blog.
- VACUUM FULL, CLUSTER, plain REPACK drop rows a no-feedback standby snapshot needs (t/012_standby_safe_rewrite.pl:81). Track: README note; possible later feature.
- TRUNCATE and rewriting ALTER TABLE read as empty to old snapshots (expected/known_gaps.out:14, :30); documented upstream. Track: none needed beyond a "not in scope" line.

## Status (2026-11-22)

Commits since the walk, on record-based-guard, none pushed: 053b4e4 (A), 7d39364 (P, Y, E), 67cf120 (R, S),
ce66b91 (V), 76f2b1f (AH), 9ba5cef (B, AL, X), 9e86441 (record not sent to subscribers), 5f16048 (AC),
a1c1482 (shared lock helper), bfeb3f6 (AJ), 91fb05b (AI), 729f34c (CI). Last full run on 19rc2: 7 regress,
8 isolation, 17 TAP files (185 tests), all passing.

The closing grouping above is as it stood on 2026-11-21. What is left:

**Before publishing**
- AN + C: run the CI workflow once (it needs a push) and fix what it finds.
- AF: done for the test run (19.0, 2026-11-22); the source line citations were not re-checked one by one.
- G: tone and contents of doc/research before the repository is public (developer is reviewing).
- D + J: push, make public, tag.

**Document or surface** (unchanged from the walk unless noted)
- W, M, N, H, K, L, AB, AE, AG, Q, O, AA, Z, T, AD.
- AB now also lacks a measurement of the cleaner's cost at the swap.

**Test gaps**
- AM: the concurrent-deleter path (needs injection points).
- K: old-snapshot UPDATE/DELETE has no permanent test.
- C: a build without TAP skips 17 test files with only a notice.

**Accepted**
- I, U, F, AJ, AK, AO, and the two stock items (foreign-key error class, replay-start window).

