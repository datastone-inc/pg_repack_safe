# REPACK (CONCURRENTLY): how it came to be not MVCC-safe

Research notes, 2026-11-15. Tags: **[V]** verified against a quoted primary
source, **[I]** inferred, **[NF]** not found. Git evidence is from the
PostgreSQL repository (REL_19_STABLE db9095b9026, origin/master to
2026-11-14). Mail evidence is from the postgresql.org archive; every
message-id below was read there.

**Summary.** The wrong-results behaviour was a known, discussed and documented
trade-off. The author first intended the in-core version to be MVCC-safe; in
April 2025 that work was set aside for performance, complexity and schedule,
and deferred to "later". An MVCC-safe patch is under review for PG20 and is not
committed. The extension precedent was cited and was contested on-list.

## 1. Lineage

- [V] REPACK CONCURRENTLY descends from pg_squeeze. Commit 28d534e2ae0: "The
  design and most of the code are by Antonin Houska, heavily based on his own
  pg_squeeze third-party implementation."
- [V] Houska wrote and maintains pg_squeeze (5367.1770017148@localhost,
  2026-02-02). Cybertec did not object to the reuse (5186.1706694913@antos,
  2024-01-31). A "significant portion of this patch is copied from the
  pg_squeeze extension" (137668.1768235610@localhost); Houska later said he
  "copied the bug to REPACK" (67772.1772462341@localhost). The bug is the
  MVCC-unsafe behavior: REPACK rewrites heap tuples under the rewriter's XID,
  so a transaction with an older snapshot can see missing rows or otherwise
  get wrong results.
- [V] Relation to pg_repack: pg_squeeze's README says it tries to replace
  pg_repack, using logical decoding where pg_repack uses triggers. Herrera
  (<202507262156.sb455angijk6@alvherre.pgsql>): users "resorted to using the
  pg_repack third-party tool, which is ancient and uses a weird internal
  implementation, as well as pg_squeeze ... The patch ... is based on the
  pg_squeeze code". REPACK replaces pg_repack; it does not descend from it.
- [V] pg_repack is "a fork of the previous pg_reorg project" (pg_repack.rst).
- Secondary sources often blur this. Spot check of 9 PG19 write-ups,
  2026-11-15 (8 read, 1 returned 403):
  - [V] attributes it to pg_repack: jusdb.com, "PostgreSQL 19 Beta: Every New
    Feature That Matters to DBAs"
    (<https://www.jusdb.com/blog/postgresql-19-beta-new-features-dba-guide>):
    "the same pattern `pg_repack` pioneered, now baked into core".
  - [V] wrong about pg_repack's mechanism: neon.com
    (<https://neon.com/postgresql/postgresql-19/repack-command>) says of
    pg_repack and REPACK "Both use logical decoding for online mode";
    pg_repack uses triggers.
  - [V] attributes it correctly to pg_squeeze: planetscale.com
    (<https://planetscale.com/blog/whats-new-in-postgres-19>): "essentially
    `pg_squeeze` brought into Postgres core".
  - The other five name pg_repack only as an alternative or make no lineage
    claim; none says pg_repack or pg_squeeze shares the MVCC caveat.
  - [NF] One explicit misattribution in this sample. "Repeatedly" needs a
    wider survey before the README claims it.
- [V] The PG19 release notes list REPACK first of seven highlights
  (`release-19.sgml`, § `release-19-highlights`, REL_19_0): "New REPACK
  command that reclaims disk space and reorganizes table contents ... REPACK
  (CONCURRENTLY) allows repacking without blocking reads and writes to the
  table." The notes do not say the list is ranked. [V] The release notes do
  not mention the MVCC caveat anywhere (no match for "mvcc" in
  `release-19.sgml`); the warning appears only in `repack.sgml` and
  `mvcc.sgml`.

## 2. Timeline: PG19 introduction, PG20 fix proposal, and `pg_repack_safe`

This timeline follows the feature from its initial proposal through PG19,
then through the separate PG20 effort to restore MVCC safety and the current
`pg_repack_safe` alternative documented here.

| Date | Event | Source |
| --- | --- | --- |
| 2024-01-30 | Pavel Stehule: "why there is not VACUUM FULL CONCURRENTLY?" | <CAFj8pRDK89FtY_yyGw7-MW-zTaHOCY4m6qfLRittdoPocz+dMQ@mail.gmail.com> [V] |
| 2024-01-31 | Houska offers pg_squeeze as the basis, says core should handle visibility | 5186.1706694913@antos [V] |
| 2024-07-09 | First patch (v01) | 82651.1720540558@antos [V] |
| 2024-07-10 | Commitfest 5117 "VACUUM FULL / CLUSTER CONCURRENTLY"; author Houska, reviewers Herrera, Nikalayeu; committer Herrera | commitfest.postgresql.org/patch/5117 [V] |
| 2025-07-26 | Herrera: "Adding REPACK [concurrently]" | <202507262156.sb455angijk6@alvherre.pgsql> [V] |
| 2026-03-10 | ac58465e061 "Introduce the REPACK command" (Herrera; author Houska) | git [V] |
| 2026-04-06 | 28d534e2ae0 "Add CONCURRENTLY option to REPACK" (Herrera; author Houska, co-authors Nikalayeu, Herrera). Adds the "not MVCC-safe" text to repack.sgml and the mvcc.sgml caveat | git [V] |
| 2026-04 to 2026-11 | ~60 follow-up commits; e.g. 04bb1500253 blocks user catalog tables as "not an MVCC-safe operation" | git [V] |
| 2026-06-16 | Houska: "REPACK enhancements" for PG20; patch 0008 "Make REPACK CONCURRENTLY MVCC safe"; CF 6957 | 109367.1781614382@localhost [V] |
| 2026-08-25 | Robert Haas ranks v19 patches by post-feature-freeze bug fixes ("scary patch contest"); REPACK is #2, "28 fixes including data loss". The thread does not mention MVCC safety | <CA+Tgmob9NY6m0YNFTQ4nFH2d0iC9SQRruDYxfndGKKzh8OC80w@mail.gmail.com> [V] |
| 2026-11-10 | Houska: v05 of the series; the MVCC-safety patch is 0009 (see §6) | 88740.1794308080@chrudos [V] |
| 2026-11-21 | MVCC-safety patch not on master | git log --all [V] |
| 2026-11-22 | This repository's research compares the proposed core fix with the `pg_repack_safe` extension, which raises an error rather than returning rows from an unsafe snapshot | §6; [fix-design.md](fix-design.md) [V] |

## 3. How the non-MVCC-safety was justified

**Original intent: MVCC-safe [V].** Houska, 2024-01-31 (5186.1706694913@antos):
"One thing that pg_squeeze does not handle is visibility: it uses
heap_insert() ... The in-core implementation should rather do something like
tuple rewriting (rewriteheap.c)." Patch versions v02-v12 carried "Preserve
visibility information of the concurrent data changes".

**Set aside, April 2025 [V].**

- Houska, 2025-04-01 (178741.1743514291@localhost), after decoding slowed
  from 3521 ms to 6561 ms: "Or implement REPACK CONCURRENTLY as MVCC-unsafe for
  now? (The pg_squeeze extension also is not MVCC-safe.)"
- Herrera, 2025-04-04 (<202504040733.ysuy5gad55md@alvherre.pgsql>): "the case
  for treating it as concurrent-safe is not credible, at least not at this
  stage -- not only because of this performance impact, but also because of
  the additional code complexity ... put that patch aside for now, maybe with a
  doc warning ... and then address that aspect later on, after this feature
  has gone through some battle-hardening."
- Houska, 2025-08-21 (40729.1755799624@localhost): "I abandoned the concept
  for now, and v13 uses plain heap_insert()."

**Challenged on-list [V].** Mihail Nikalayeu:

- 2025-08-04: "this is a significant limitation for practical use."
- 2025-08-09: "For multiple applications, this is a dealbreaker, because in
  some cases correctness is a higher priority than availability." Proposed
  terminating connections with old snapshots, or a CIC-style wait "until older
  snapshots no longer treat XID1 as uncommitted" (CIC is `CREATE INDEX
  CONCURRENTLY`, which waits out older snapshots before it finishes).
- 2025-08-29: argued for MVCC-safe by default or "REPACK (CONCURRENTLY,
  UNSAFE)"; "'CONCURRENTLY' is something that feels like a 'working in
  background, don't worry...' thing, especially in Postgres because of CIC."

**Responses [V].**

- Herrera, 2025-08-09 (<202508091333.qvgvo7ikuezm@alvherre.pgsql>): "having an
  MVCC-safe mode has drawbacks, IMO we should make it optional."
- Herrera, 2025-08-28 (<202508282139.j24af7vwn7pz@alvherre.pgsql>): no time to
  get it "reviewed and committed in time for 19"; agreed it would be
  `REPACK (CONCURRENTLY, SAFE)`.
- Houska, 2025-08-27 (17670.1756289505@localhost): "It does not really worry
  me. The pg_squeeze extension is not MVCC-safe and I remember there were only
  1 or 2 related complaints ... (pg_repack isn't MVCC-safe as well...) Of
  course, user documentation should warn about the problem, in a way it does
  for other commands (typically ALTER TABLE)."
- Paquier, 2025-09-01 (<aLTl6mYK7exHnqUJ@paquier.xyz>): favoured "a basic
  implementation" first.

**Error-instead-of-wrong-result was proposed and rejected [V].** This is the
reader-side design in [fix-design.md](fix-design.md).

- Nikalayeu, 2026-01-25 (<CADzfLwUEH5+LjCN+6kRfSsXwuou8rKXyVV42Wi-O_TG0360Kug@mail.gmail.com>):
  `relcheckxmin`, modelled on indcheckxmin, "causes the executor to raise an
  error if a transaction with an older snapshot attempts to access the
  rewritten relation."
- Herrera, 2026-02-01 (<202602012202.qo2k6ip7hoer@alvherre.pgsql>): "I'm not
  sure it's acceptable to cause other sessions to raise errors ... Imagine a
  long-running transactions ... being killed near the end because some DBA
  decided to run REPACK".
- Nikalayeu: "it just changes from 'see an empty table because of MVCC
  violation' to 'get error' in the exact situation. I prefer the second." Not
  adopted.
- Houska, 2026-01-26 (3901.1769412880@localhost): his MVCC-safe prototype was
  itself incorrect (HOT chains). "Let's see in the future if the demand for the
  MVCC safety will ever justify the effort to implement it."

**Comparisons made [V].** The mvcc.sgml caveat groups REPACK CONCURRENTLY with
TRUNCATE and rewriting ALTER TABLE. "Same as VACUUM FULL/CLUSTER" was not
argued and would be false: Houska, 2025-08-25 (23676.1756140852@localhost),
"Both CLUSTER and VACUUM FULL already do that" (keep visibility information).

**Conclusion [V].** Concurrency was the primary feature. Wrong results were
accepted knowingly, documented, and treated as temporary, for performance,
complexity and committer time before the v19 freeze.

**Reports after release candidates [V].**

- pgsql-bugs, 2026-08-22 (<5d2060d5-8145-40c6-9a2e-4ddb9f6b0fc9@jbitc.de>):
  REPEATABLE READ count(*) returned 0. Geoghegan: "This is a documented
  behavior; 'repack concurrently' is allowed to violate MVCC."
- pgsql-hackers, 2026-09-25, Thom Brown, "REPACK (CONCURRENTLY) can lose data
  in pg_dump output" (<CAA-aLv57B5g3aiGodzkuePa8-7KSQz7_5rspbPDY-7XPCBz0Tw@mail.gmail.com>):
  empty COPY, exit 0; asked whether pg_dump could detect the rewrite and fail.
  Imseih (<CAN12+YLfcPihppiFSEr6+5JD3Hs_uhKKCp4FNOb8GTVJ-XQ9uA@mail.gmail.com>):
  expected for any rewrite; showed a duplicate-key error after seeing 0 rows,
  and noted STABLE/IMMUTABLE functions are affected under READ COMMITTED.

**Doc inconsistency.** repack.sgml says "snapshot taken before repacking
started"; mvcc.sgml says "before the command committed". This repo's
`t/001_repack_pg_dump.pl` case 2 shows the mvcc.sgml wording is the accurate
one.

## 4. pg_repack and pg_squeeze have the same caveat

- [V] pg_squeeze documents it: "Like pg_repack, pg_squeeze also changes
  visibility of rows and thus allows for MVCC-unsafe behavior". Issue #60
  (2023): an UPDATE missed rows; Houska: fixing it would need tuple rewriting,
  "no plan to introduce it".
- [V] pg_repack behaves the same (copies with INSERT ... SELECT, then
  `repack_swap()`; issue #79: "no practical way to preserve the xmin"), but its
  documentation never mentions MVCC, snapshots or visibility.
- [NF] No pg_repack issue reporting wrong results was found (search was
  partly rate-limited).

## 5. The role of the pg_repack and pg_squeeze precedent

For the framing [V]:

- The precedent was cited at the decision point (Houska 2025-04-01) and its
  complaint history used as reassurance (2025-08-27).
- Herrera, on a related limitation (<202604011042.zdevjay65ws7@alvherre.pgsql>):
  "it's better to *have* the tool, even if it has problems ... pg_repack and
  pg_squeeze also have all these problems/limitations (and others), and still
  people use them."
- Objected to at the time, nearly in this repo's terms. Nikalayeu, 2025-08-27:
  "But pg_squeeze and pg_repack are extensions. If we are moving that mechanics
  into core I'd expect some improvements over pg_squeeze."
- Plageman, "scary patch contest", 2026-08-26
  (<CAAKRu_YFSZ_6wQR52UyHU5SmwNLnLdteCautz-KdC1Vv-5mBdw@mail.gmail.com>):
  "Being somewhat better than very buggy 3rd party solutions is not the bar for
  what goes into Postgres." (Not arguing for a revert.)

Against or complicating it [V]:

- Herrera's deciding message (2025-04-04) gives performance, complexity and
  timeline, not the extensions.
- The author held that core should do better and has an MVCC-safe patch for
  PG20.
- Core precedent (TRUNCATE, ALTER TABLE) was also invoked.
- "Users knowingly accepted it" holds for pg_squeeze (documented), weakly for
  pg_repack (undocumented).

Verdict: the precedent was a stated input alongside cost, complexity and
schedule, contested at the time; the record does not show it was decisive.

## 6. The PG20 patch compared with this extension

Added 2026-11-22. Patch read: v05-0009 "Make REPACK CONCURRENTLY MVCC safe"
(88740.1794308080@chrudos, 2026-11-10; numbered 0008 in v01 to v04). Roughly
the first third of the patch text was read verbatim and the rest through a
summary, so patch details are **[V]** only where they come from the commit
message or that first third, and **[I]** otherwise. PG19 facts were checked
in the REL_19_0 tree.

**Different kinds of fix.** The patch removes the cause; the extension detects
the wrong read and raises an error. The patch is candidate F in
[fix-design.md](fix-design.md); the extension is candidate G.

| | pg_repack_safe | PG20 patch |
| --- | --- | --- |
| Mechanism | The writer records each unsafe rewrite; a reader errors (40001) if its snapshot does not see the rewriter's XID as committed | The new heap keeps each row's original xmin/xmax instead of the REPACK XID |
| Old-snapshot reader | Error | Correct rows |
| Standbys | Need the library; a custom WAL record stops replay without it | Nothing special: the WAL carries the original XIDs |
| Known gaps | Readers that bypass the guarded paths (direct-scan extensions, output plugins) | None by design |
| Option or default | Loaded per cluster, created per database | Changes the command itself; no SAFE option seen [I] |

**What the patch changes.**

- [V] Commit message: "This patch teaches REPACK (CONCURRENTLY) to transfer
  the visibility information from the old table to the new one."
- [V] `heap_insert` and `heap_update` take a new flag `TABLE_REUSE_XID` and
  then use the tuple's own xmin instead of `GetCurrentTransactionId()`.
  `heap_delete`, `simple_heap_delete`, the table AM `tuple_delete` callback
  and the TOAST delete functions gain a `TransactionId xid` argument.
- [V] The copy goes through rewriteheap.c: `heap_insert_for_repack` calls a
  new `rewrite_heap_tuple_no_chains`, and `begin_heap_rewrite` gains a
  `no_chains` argument. TOAST tuples are inserted with `raw_heap_insert`.
- [V] WAL: a record flag `XLR_XID_REPLAYED`, a call to set the record's XID,
  and a bumped `XLOG_PAGE_MAGIC`. `decode.c` returns early for flagged
  records.
- [V] A new snapshot type `SNAPSHOT_NEW_HEAP` with
  `HeapTupleSatisfiesNewHeap` finds existing tuples in the new heap when
  concurrent changes are applied.
- [V] The command commits before copying, "so that there is no XID assigned
  while we copy the data" (cover letter, 109367.1781614382@localhost), and
  copies in block ranges.
- [I] An auxiliary table with an identity index replaces the tuplestore.
  Diffstat: 33 files, 1223 insertions, 336 deletions.
- [V] Stated limitation: "we only freeze tuples during the initial copying,
  while the data changes decoded from WAL are replayed without freezing."

**Not implementable as an extension [V, PG19 source].**

- `heapam.c (heap_insert)`, `(heap_update)` and `(heap_delete)` take the XID
  from `GetCurrentTransactionId()`, with no argument or hook to override it.
- `heapam_handler.c (heap_insert_for_repack)` and
  `rewriteheap.c (raw_heap_insert)` are `static`.
- `repack.c` and `repack_worker.c` contain no hooks.
- The WAL record flag, the page magic, the decode.c skip and the new snapshot
  type are core changes with no extension point.
- [I] The remaining extension route is a replacement command with private
  copies of the heap insert, update and delete code, as pg_squeeze is built.
  That does not make stock `REPACK (CONCURRENTLY)` safe, and pg_squeeze
  itself never preserved visibility (§4).

**State of the patch.**

- [V] Not on origin/master as of 2026-11-21 (`git log --all --grep`).
- [I] Commitfest 6957, from a possibly stale page: "Needs review", needing a
  rebase, author Houska, reviewer Reyes Bravo, no committer.
- [V] Open at v05: an assertion in `compute_new_xmax_infomask` is commented
  out; Houska: "I couldn't reproduce this problem yet."
- [I] Open at v05: a failed run leaves a `pg_temp_<oid>` table that breaks
  later rewrites and `pg_upgrade` (Reyes Bravo, 2026-09-22); no fix seen.
- [I] Reported fixed in v04: rows lost when the table grows during the run,
  and a wraparound bug with synchronized seqscans (shihao zhong, 2026-09).
  A "could not find target tuple" error after a concurrent DELETE or UPDATE
  has a fix in v05 that was not seen confirmed.
- [I] No back-patch to 19 has been proposed, and the WAL format change rules
  one out.

## Source caveats

- The v05-0009 patch (§6) was read verbatim only for about its first third;
  the 2026-09-24 and 2026-11-11 to 2026-11-14 thread messages were read only
  through a summary.
- PGConf.EU 2025 slides are image-only; nothing in them is verified.
- [V] PG19 GA date 2026-11-19: `release-19.sgml` at REL_19_0, "Release
  date: 2026-11-19".
- The Phoronix write-up returned 403 and was not read.
