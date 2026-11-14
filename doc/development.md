# Development Guide

## Prerequisites

- PostgreSQL 19 with development headers
- GCC or Clang with C99 support
- Git
- Make

## Clone and Build

```bash
git clone https://github.com/datastone-inc/pg_repack_safe.git
cd pg_repack_safe
make
```

## Testing

Regression tests use `pg_regress` through PGXS. `sql/` holds the test
scripts and `expected/` the output each must produce.

```bash
make install
make installcheck
```

A failing run leaves `regression.diffs` in the repository root.

Every SQL example in the documentation has a matching test in
`sql/doc_examples.sql` (see `project_template.md`).
