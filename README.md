<!-- markdownlint-disable MD033 MD041 -->
<div align="center">

<img src="assets/datastone-logo.svg" alt="dataStone Logo" width="400"/>

# pg_repack_safe

[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-19-blue.svg)](https://www.postgresql.org/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-C-orange.svg)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Extension](https://img.shields.io/badge/Type-PostgreSQL%20Extension-purple.svg)](https://www.postgresql.org/docs/current/extend-extensions.html)

PostgreSQL extension (description to come)

</div>
<!-- markdownlint-enable MD033 MD041 -->

---

## Overview

The extension is under construction. With the library in
`shared_preload_libraries` and the extension created, it prevents the
documented wrong result for queries: a query whose snapshot predates a
committed `REPACK (CONCURRENTLY)` of a table it reads gets an error instead
of an empty table. It does **not** yet cover `COPY ... TO` (and so
`pg_dump`), standbys, or the other cases listed in
[doc/research/fix-design.md](doc/research/fix-design.md), and records are
never cleaned up yet.

What exists so far:

- a loadable library whose hooks detect when a table's storage is replaced
  and whether `REPACK (CONCURRENTLY)` did it;
- the writer: with the library loaded, `REPACK (CONCURRENTLY)` of a table
  that has data writes a record, (table, rewriting transaction ID), into the
  extension's table `pg_repack_safe_rewrites`, plus one for the table's TOAST
  relation. `REPACK (CONCURRENTLY)` is refused in a database where the
  extension is not created, and when its record table cannot be found or has
  been redefined;
- the reader check: once a query holds its locks, it raises
  `ERROR: cannot read table "..." with this snapshot` (SQLSTATE 40001, a
  serialization failure: retry the transaction) when a table it reads has a
  record whose `REPACK (CONCURRENTLY)` committed but is not visible to the
  query's snapshot. `EXPLAIN` without `ANALYZE` is not checked, since it
  reads no rows. Queries are also refused if the extension exists but the
  record table has been renamed, moved to another schema, or dropped. That second refusal is off while the server runs in
  binary-upgrade mode (`pg_upgrade`, or a server started by hand with `-b`),
  because `pg_upgrade` restores the extension before its table.
  `REPACK (CONCURRENTLY)` is refused whenever the extension exists and the
  table cannot be found, in that mode too.

The design is in [doc/research/fix-design.md](doc/research/fix-design.md).

## Installation

```bash
make
sudo make install        # omit sudo when the PostgreSQL install is yours
```

See [doc/installation.md](doc/installation.md).

## Quick Start

```sql
CREATE EXTENSION pg_repack_safe;
```

## Documentation

- [Installation Guide](doc/installation.md)
- [Development Guide](doc/development.md)

## License

MIT; see [LICENSE](LICENSE).
