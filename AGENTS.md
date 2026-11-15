# pg_repack_safe: agent guidelines

This repo makes PostgreSQL 19's `REPACK ... CONCURRENTLY` safe to run. Stock
PG19 lets that command give wrong query results, and this extension closes the
gap with a C extension, server hooks, or both. **When it is unclear whether a
query will see correct results, refuse the operation.**

## The problem

Stock PG19 documents `REPACK` with `CONCURRENTLY` as not MVCC-safe
(`/opt/pg19/src/doc/src/sgml/ref/repack.sgml`, the `<warning>` near line 321;
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
     claim. Back it with a `file:line` in the PG19 source or a test that
     exercises it, in the same breath. Otherwise list it as an open question.
   - *Declining to fix.* "Out of scope", "can't happen in practice" and
     "upstream's problem" are divergences too, and they are the claims nobody
     re-checks. Give the evidence right away or put the item on the fix list.
   - *Known gaps.* A wrong-results case the extension does not yet close is
     written down in `doc/` with a reproducer, never left implicit.
   - *PG internals.* Read the PG19 source (`/opt/pg19/src`, mainly
     `src/backend/commands/repack.c` and `repack_worker.c`) before asserting
     how `REPACK` behaves. Cite the line. Check memory against the code.

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

   Run against PG19:
   `. ~/dev/datastone/pg19/env.sh && make && make install && make installcheck`.
   The default `pg_config` on `PATH` is PG18, which will not do.

6. **No multi-step work without a per-step verification plan.**

   Before implementing anything with more than one step, break it down and
   give each step an observable check that must pass before the next one
   starts: a failing-then-passing regress or isolation test, or a manual
   probe with its expected output. Don't mark a step done until its check
   passes.

7. **Style.**

   Follow the dataStone extension constitution
   (`~/dev/pg-extension-template/.specify/memory/constitution.md`): Allman
   braces, 4-space indent, no tabs. Internal C symbols are camelCase and
   SQL-callable C functions are snake_case. SQL keywords are UPPERCASE. Every
   C and SQL file carries the header `Copyright (c) 2026 dataStone Inc.` +
   `SPDX-License-Identifier: MIT` + a doxygen `@file`/`@brief` block. Function
   headers use `@brief`/`@param`/`@return`.

   Comments describe the code as it is now, not the change that produced it
   (no "added for...", "was previously..."). They state the invariant and
   why. The history of a change goes in the commit message. Deferred work is
   marked `TODO(owner YYYY-MM-DD): ...`.

## Documentation maintenance

- `README.md` explains what is unsafe in stock PG19, what this extension
  guarantees, and what it does not yet guarantee. Keep all three in sync with
  the code.
- `CHANGELOG.md` follows Keep a Changelog. Every user-visible change gets an
  `[Unreleased]` entry.
- Repo layout and doc-file conventions: `project_template.md`.

## Target versions

- PostgreSQL 19 (local build `/opt/pg19`, currently 19rc2, source at
  `/opt/pg19/src`). `REPACK` does not exist before 19. Don't add
  compatibility shims for older majors.
- Release state: prerelease. Edit `pg_repack_safe--1.0.0.sql` in place. No
  version bumps or upgrade scripts until the first release.
