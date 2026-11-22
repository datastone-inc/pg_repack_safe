# REPACK (CONCURRENTLY): history and how the wrong results were accepted

Research notes, 2026-11-15. Tags: **[V]** verified against a quoted primary
source, **[I]** inferred, **[NF]** not found. Git evidence is from the
PostgreSQL repository (REL_19_STABLE db9095b9026, origin/master to
2026-11-14). Mail evidence is from the postgresql.org archive; every
message-id below was read there.

**Summary.** The wrong-results behaviour was a known, discussed and documented
trade-off. The author first intended the in-core version to be MVCC-safe; in
April 2025 that work was set aside for performance, complexity and schedule,
and deferred to "later". An MVCC-safe patch is under review for PG20 and is not
committed. The extension precedent was cited and was contested on-list. Nothing
links AI-generated patches or reviews to the REPACK review or the MVCC decision.

## 1. Lineage

- [V] REPACK CONCURRENTLY descends from pg_squeeze. Commit 28d534e2ae0: "The
  design and most of the code are by Antonin Houska, heavily based on his own
  pg_squeeze third-party implementation."
- [V] Houska wrote and maintains pg_squeeze (5367.1770017148@localhost,
  2026-02-02). Cybertec did not object to the reuse (5186.1706694913@antos,
  2024-01-31). A "significant portion of this patch is copied from the
  pg_squeeze extension" (137668.1768235610@localhost); he later "copied the bug
  to REPACK" (67772.1772462341@localhost).
- [V] Relation to pg_repack: pg_squeeze's README says it tries to replace
  pg_repack, using logical decoding where pg_repack uses triggers. Herrera
  (202507262156.sb455angijk6@alvherre.pgsql): users "resorted to using the
  pg_repack third-party tool, which is ancient and uses a weird internal
  implementation, as well as pg_squeeze ... The patch ... is based on the
  pg_squeeze code". REPACK replaces pg_repack; it does not descend from it.
- [V] pg_repack is "a fork of the previous pg_reorg project" (pg_repack.rst).
- Secondary sources often blur this. Spot check of 9 PG19 write-ups,
  2026-11-15 (8 read, 1 returned 403):
  - [V] attributes it to pg_repack: jusdb.com, "PostgreSQL 19 Beta: Every New
    Feature That Matters to DBAs"
    (https://www.jusdb.com/blog/postgresql-19-beta-new-features-dba-guide):
    "the same pattern `pg_repack` pioneered, now baked into core".
  - [V] wrong about pg_repack's mechanism: neon.com
    (https://neon.com/postgresql/postgresql-19/repack-command) says of
    pg_repack and REPACK "Both use logical decoding for online mode";
    pg_repack uses triggers.
  - [V] attributes it correctly to pg_squeeze: planetscale.com
    (https://planetscale.com/blog/whats-new-in-postgres-19): "essentially
    `pg_squeeze` brought into Postgres core".
  - The other five name pg_repack only as an alternative or make no lineage
    claim; none says pg_repack or pg_squeeze shares the MVCC caveat.
  - [NF] One explicit misattribution in this sample. "Repeatedly" needs a
    wider survey before the README claims it.
- "pg_swap": [NF] no table-rewrite extension of that name exists (web and
  GitHub search). [V] pg_repack has an internal function
  `repack.repack_swap(oid)`. [I] The name is most likely a mix-up of that
  function or of pg_squeeze.

## 2. Timeline

| Date | Event | Source |
|---|---|---|
| 2024-01-30 | Pavel Stehule: "why there is not VACUUM FULL CONCURRENTLY?" | CAFj8pRDK89FtY_yyGw7-MW-zTaHOCY4m6qfLRittdoPocz+dMQ@mail.gmail.com [V] |
| 2024-01-31 | Houska offers pg_squeeze as the basis, says core should handle visibility | 5186.1706694913@antos [V] |
| 2024-07-09 | First patch (v01) | 82651.1720540558@antos [V] |
| 2024-07-10 | Commitfest 5117 "VACUUM FULL / CLUSTER CONCURRENTLY"; author Houska, reviewers Herrera, Nikalayeu; committer Herrera | commitfest.postgresql.org/patch/5117 [V] |
| 2025-07-26 | Herrera: "Adding REPACK [concurrently]" | 202507262156.sb455angijk6@alvherre.pgsql [V] |
| 2026-03-10 | ac58465e061 "Introduce the REPACK command" (Herrera; author Houska) | git [V] |
| 2026-04-06 | 28d534e2ae0 "Add CONCURRENTLY option to REPACK" (Herrera; author Houska, co-authors Nikalayeu, Herrera). Adds the "not MVCC-safe" text to repack.sgml and the mvcc.sgml caveat | git [V] |
| 2026-04 to 2026-11 | ~60 follow-up commits; e.g. 04bb1500253 blocks user catalog tables as "not an MVCC-safe operation" | git [V] |
| 2026-06-16 | Houska: "REPACK enhancements" for PG20; patch 0008 "Make REPACK CONCURRENTLY MVCC safe"; CF 6957 | 109367.1781614382@localhost [V] |
| 2026-11-14 | MVCC-safety patch not on master | git log --all [V] |

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
- Herrera, 2025-04-04 (202504040733.ysuy5gad55md@alvherre.pgsql): "the case
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
  snapshots no longer treat XID1 as uncommitted".
- 2025-08-29: argued for MVCC-safe by default or "REPACK (CONCURRENTLY,
  UNSAFE)"; "'CONCURRENTLY' is something that feels like a 'working in
  background, don't worry...' thing, especially in Postgres because of CIC."

**Responses [V].**
- Herrera, 2025-08-09 (202508091333.qvgvo7ikuezm@alvherre.pgsql): "having an
  MVCC-safe mode has drawbacks, IMO we should make it optional."
- Herrera, 2025-08-28 (202508282139.j24af7vwn7pz@alvherre.pgsql): no time to
  get it "reviewed and committed in time for 19"; agreed it would be
  `REPACK (CONCURRENTLY, SAFE)`.
- Houska, 2025-08-27 (17670.1756289505@localhost): "It does not really worry
  me. The pg_squeeze extension is not MVCC-safe and I remember there were only
  1 or 2 related complaints ... (pg_repack isn't MVCC-safe as well...) Of
  course, user documentation should warn about the problem, in a way it does
  for other commands (typically ALTER TABLE)."
- Paquier, 2025-09-01 (aLTl6mYK7exHnqUJ@paquier.xyz): favoured "a basic
  implementation" first.

**Error-instead-of-wrong-result was proposed and rejected [V].** This is the
reader-side design in [fix-design.md](fix-design.md).
- Nikalayeu, 2026-01-25 (CADzfLwUEH5+LjCN+6kRfSsXwuou8rKXyVV42Wi-O_TG0360Kug@mail.gmail.com):
  `relcheckxmin`, modelled on indcheckxmin, "causes the executor to raise an
  error if a transaction with an older snapshot attempts to access the
  rewritten relation."
- Herrera, 2026-02-01 (202602012202.qo2k6ip7hoer@alvherre.pgsql): "I'm not
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
- pgsql-bugs, 2026-08-22 (5d2060d5-8145-40c6-9a2e-4ddb9f6b0fc9@jbitc.de):
  REPEATABLE READ count(*) returned 0. Geoghegan: "This is a documented
  behavior; 'repack concurrently' is allowed to violate MVCC."
- pgsql-hackers, 2026-09-25, Thom Brown, "REPACK (CONCURRENTLY) can lose data
  in pg_dump output" (CAA-aLv57B5g3aiGodzkuePa8-7KSQz7_5rspbPDY-7XPCBz0Tw@mail.gmail.com):
  empty COPY, exit 0; asked whether pg_dump could detect the rewrite and fail.
  Imseih (CAN12+YLfcPihppiFSEr6+5JD3Hs_uhKKCp4FNOb8GTVJ-XQ9uA@mail.gmail.com):
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

## 5. Did the extension precedent cloud the judgement?

For the framing [V]:
- The precedent was cited at the decision point (Houska 2025-04-01) and its
  complaint history used as reassurance (2025-08-27).
- Herrera, on a related limitation (202604011042.zdevjay65ws7@alvherre.pgsql):
  "it's better to *have* the tool, even if it has problems ... pg_repack and
  pg_squeeze also have all these problems/limitations (and others), and still
  people use them."
- Objected to at the time, nearly in this repo's terms. Nikalayeu, 2025-08-27:
  "But pg_squeeze and pg_repack are extensions. If we are moving that mechanics
  into core I'd expect some improvements over pg_squeeze."
- Plageman, "scary patch contest", 2026-08-26
  (CAAKRu_YFSZ_6wQR52UyHU5SmwNLnLdteCautz-KdC1Vv-5mBdw@mail.gmail.com):
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

## 6. The "AI slop" angle

- [V] Robert Haas, 2026-10-21, "Postgres 19 is late and it's AI's fault"
  (https://neon.com/blog/postgres-19-is-late-and-its-ais-fault): "LLMs
  happened" — bugs became far easier to find, diverting reviewer time. REPACK
  is not mentioned.
- [V] "scary patch contest" (CA+Tgmob9NY6m0YNFTQ4nFH2d0iC9SQRruDYxfndGKKzh8OC80w@mail.gmail.com,
  2026-08-25): Haas used Claude to rank v19 patches by post-freeze bug fixes;
  REPACK ranked #2 ("28 fixes including data loss"). Kapila: the extra bugs
  "could be both due to AI and the feature is committed near to feature-freeze".
  MVCC safety is not mentioned in the thread.
- [NF] No evidence that AI-generated patches or reviews were part of REPACK's
  review or influenced the MVCC decision, which was made in April 2025 for the
  reasons in §3. No message in the REPACK threads mentions an LLM; no REPACK
  commit carries an AI disclosure. **The README must not suggest a
  connection.**

## Source caveats

- PGConf.EU 2025 slides are image-only; nothing in them is verified.
- PG19 GA date (Nov 19, per press) not checked against a primary source.
- The Phoronix write-up returned 403 and was not read.

## Draft README section

The last paragraph is a placeholder until the design decision in
[fix-design.md](fix-design.md) is made: whether the extension refuses the
REPACK, errors in the reader, or both.

### Background: why REPACK CONCURRENTLY can return wrong results

PostgreSQL 19 adds `REPACK`, one command that takes over from `VACUUM FULL`
and `CLUSTER` (commit ac58465e061), and a `CONCURRENTLY` option (commit
28d534e2ae0). With the option, the new copy of the table is built under a
`SHARE UPDATE EXCLUSIVE` lock, changes made meanwhile are captured by logical
decoding and replayed onto the copy, and an exclusive lock is taken only for
the final swap. The design and most of the code are "heavily based on" the
pg_squeeze extension (Antonin Houska, Cybertec), not on pg_repack, although
it is often described as pg_repack moved into core. pg_repack uses triggers;
pg_squeeze and REPACK use logical decoding.

The PostgreSQL documentation says: "`REPACK` with the `CONCURRENTLY` option is
not MVCC-safe." The new copy's rows are written with the repacking
transaction's ID, not their original `xmin`. After the swap commits, any
transaction whose snapshot was taken before that commit sees the table as
empty. Plain `REPACK`, `VACUUM FULL` and `CLUSTER` preserve visibility and are
not affected.

None of the following raises an error; the result is simply wrong:

- a `REPEATABLE READ` transaction counts 0 rows;
- a cursor at the default `READ COMMITTED` level gets NULL from a `STABLE`
  function that looks up the repacked table;
- `pg_dump` writes an empty `COPY` block and exits 0, including a `pg_dump`
  that started after the REPACK did;
- a transaction that sees no rows can still hit a duplicate-key error when it
  inserts.

This trade-off was discussed and accepted during development. The original
plan was to preserve visibility; in April 2025 that part was set aside because
it slowed logical decoding and added complexity, with a documentation warning
and a plan to address it "later on, after this feature has gone through some
battle-hardening." Reviewers asked for MVCC safety by default; one proposed
that old snapshots get an error instead of an empty table, which the committer
declined because it would raise errors in other sessions. A patch to make the
command MVCC-safe is under review for PostgreSQL 20 and is not committed.

The third-party tools REPACK replaces behave the same way. pg_squeeze
documents that it "allows for MVCC-unsafe behavior"; pg_repack also gives
copied rows a new `xmin` but does not document it.

`pg_repack_safe` takes the position that a maintenance command must not
silently change query results: an error is preferable to a wrong answer.
(TODO: state the mechanism once chosen.)

**References**

1. PostgreSQL 19 docs: REPACK, and MVCC Caveats (doc/src/sgml/ref/repack.sgml, doc/src/sgml/mvcc.sgml)
2. Commits ac58465e061 (2026-03-10) and 28d534e2ae0 (2026-04-06)
3. https://www.postgresql.org/message-id/5186.1706694913@antos
4. https://www.postgresql.org/message-id/178741.1743514291@localhost
5. https://www.postgresql.org/message-id/202504040733.ysuy5gad55md@alvherre.pgsql
6. https://www.postgresql.org/message-id/CADzfLwUEH5%2BLjCN%2B6kRfSsXwuou8rKXyVV42Wi-O_TG0360Kug%40mail.gmail.com
7. https://www.postgresql.org/message-id/202602012202.qo2k6ip7hoer@alvherre.pgsql
8. https://www.postgresql.org/message-id/5d2060d5-8145-40c6-9a2e-4ddb9f6b0fc9@jbitc.de
9. https://www.postgresql.org/message-id/CAA-aLv57B5g3aiGodzkuePa8-7KSQz7_5rspbPDY-7XPCBz0Tw@mail.gmail.com
10. https://www.postgresql.org/message-id/109367.1781614382@localhost and https://commitfest.postgresql.org/patch/6957/
11. https://github.com/cybertec-postgresql/pg_squeeze (issue #60)
12. https://reorg.github.io/pg_repack/ (issue https://github.com/reorg/pg_repack/issues/79)
