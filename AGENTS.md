# pg_repack_safe: agent guidelines

This repo makes PostgreSQL 19's `REPACK ... CONCURRENTLY` safe to run. Stock
PG19 lets that command give wrong query results, and this extension closes the
gap with a C extension, server hooks, or both. **When it is unclear whether a
query will see correct results, refuse the operation.**

## The problem

Stock PG19 documents `REPACK` with `CONCURRENTLY` as not MVCC-safe
(`doc/src/sgml/ref/repack.sgml`, the `<warning>` about `CONCURRENTLY`;
`doc/src/sgml/mvcc.sgml` § "Caveats", `id="mvcc-caveats"`). After the repack
commits, a transaction whose snapshot was taken before the repack started, and
which had not yet touched the table, sees the table as **empty**. Successive
queries on the table stay consistent with each other, but the table can
disagree with the rest of the database. Nothing raises an error. The query just
returns wrong rows.

That documented case is where the work starts, and it may not be the only one.
Any other path to wrong results under `REPACK CONCURRENTLY` (logical decoding
of concurrent changes, the swap, the indexes, TOAST, replicas, prepared
transactions, serializable isolation) is an open question until a reproducer
or a PG source citation settles it.

## Core principles

1. **Correct results first. Refuse when in doubt.**

   An error, a refused `REPACK`, or a blocked or cancelled transaction is
   always acceptable. A query that silently returns wrong rows never is. When
   a design trades correctness for availability, throughput or convenience,
   refusing wins unless the developer explicitly decides otherwise, and that
   decision is recorded in the docs with its reason. A safety check that
   cannot reach a verdict (catalog state it does not recognize, a snapshot it
   cannot classify, a hook it finds already in an unexpected state) also
   refuses. It never lets the operation through.

2. **Surface unknowns. Don't resolve them silently.**

   Stop and present any open question to the developer before coding. Never
   quietly pick an interpretation, and never weaken a test to match the code.

   - *Unproven safety claims.* "This path can't produce wrong results" is a
     claim. Back it with the PG19 source or a test that exercises it, in the
     same breath. Otherwise list it as an open question.
   - *Declining to fix.* "Out of scope", "can't happen in practice" and
     "upstream's problem" are divergences too, and they are the claims nobody
     re-checks. Give the evidence right away or put the item on the fix list.
   - *Known gaps.* A wrong-results case the extension does not yet close is
     written down in `doc/` with a reproducer, never left implicit.
   - *PG internals.* Read the PostgreSQL 19 source (mainly
     `src/backend/commands/repack.c` and `repack_worker.c`) before asserting
     how `REPACK` behaves. Check memory against the code.

3. **Clean, correct design over clever design.**

   Prefer the smallest mechanism whose correctness argument you can write
   down in full. Every hook or interception states its invariant: which
   operations it catches, by meaning, not by spelling. A check that catches
   `REPACK (CONCURRENTLY) t` must also catch every other grammar form and
   every internal caller that reaches the same code path. Census the grammar
   and the callers, and test the alternate spellings. Chain to any previous
   hook value. Never assume this extension is the only one loaded.

4. **Backend safety is non-negotiable.**

   All code here runs inside the server process, where a crash takes down
   the whole server. C only, no C++. Use palloc/pfree (never malloc/free) and
   ereport/elog (never printf). Respect the `longjmp` error model: no
   resource held across an `ereport(ERROR)` without a cleanup path. Hooks
   install in `_PG_init` and must behave correctly whether the library loads
   through `shared_preload_libraries` or later. If a mechanism only works
   preloaded, check for that and refuse with a clear error when it isn't.

5. **TDD with pg_regress, and isolation tests for concurrency.**

   Write the failing test before the implementation. Wrong-results bugs here
   are concurrency bugs, so their tests are isolation specs (`specs/*.spec`,
   `expected/*.out`, run through PGXS `ISOLATION`). A spec pins the exact
   interleaving that produces wrong rows on stock PG19 and shows that the
   extension blocks or errors instead. Every closed case keeps its reproducer
   as a permanent test. Single-session behavior (refusals, GUCs, SQL
   functions) goes in `sql/` + `expected/` through `REGRESS`. Every SQL
   example in the docs has a test in `sql/doc_examples.sql`.

   Run against PostgreSQL 19, with its `pg_config` first on `PATH` or named
   in `PG_CONFIG`: `make && make install && make installcheck`. An older
   major will not do.

6. **No multi-step work without a per-step verification plan.**

   Before implementing anything with more than one step, break it down and
   give each step an observable check that must pass before the next one
   starts: a failing-then-passing regress or isolation test, or a manual
   probe with its expected output. Don't mark a step done until its check
   passes.

7. **Style.**

   Allman braces, 2-space indent, no tabs. Declarations and struct members
   are not column-aligned; a cast has no space after it (`(void)arg`). Lines
   stay within 79 columns, except user-visible message strings (errmsg,
   errdetail, errhint), which are never split so they can be found with
   grep. `.clang-format` encodes these rules: run `make format` after
   editing C, and `make check-format` (which CI runs) must pass. clang-format
   leaves comments as written, so keep them within 79 columns by hand.
   Internal C symbols are camelCase, and C functions that SQL declares
   (trigger and SQL functions) are snake_case. SQL keywords are UPPERCASE.
   Every C and SQL file carries the header `Copyright (c) 2026 dataStone
   Inc.` + `SPDX-License-Identifier: MIT` + a doxygen `@file`/`@brief`
   block. Function headers use `@brief`/`@param`/`@return`. A C file split
   into `/* ---- */` sections lists them in its `@file` block, in order, one
   line each, and that list changes when a section does.

   Comments describe the code as it is now, not the change that produced it
   (no "added for...", "was previously..."). The history of a change goes in
   the commit message. Comments state the invariant and why, once, next to
   the code that relies on it; a function header says what the function
   guarantees, and the reasoning for one branch goes in that branch. Say the
   fact itself, not a phrase that stands for it. The principles above are
   rules for the design, not wording for comments: instead of "caught by
   meaning" or "whatever statement does it", list the statements that reach
   the check. Leave out hedges ("errs towards", "rather than assumed", "not
   part of the guarantee") and sentences that repeat the code or the
   previous sentence. Deferred work is marked `TODO(owner YYYY-MM-DD): ...`.
   A reference to PostgreSQL's source names the file and the function, never
   a line number: line numbers go stale with every release.

## Documentation maintenance

- `README.md` explains what is unsafe in stock PG19, what this extension
  guarantees, and what it does not yet guarantee. Keep all three in sync with
  the code.
- `CHANGELOG.md` follows Keep a Changelog. Every user-visible change gets an
  `[Unreleased]` entry.
- Repo layout, tests and doc conventions: `doc/development.md`.

## Target versions

- PostgreSQL 19 (tested on 19.0). `REPACK` does not exist before 19. Don't
  add compatibility shims for older majors.
- Release state: prerelease. Edit `pg_repack_safe--1.0.sql` in place. No
  version bumps or upgrade scripts until the first release.
