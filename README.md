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

The extension is under construction. **It does not yet prevent any wrong
result**, and with the library loaded it is not usable on a database that
runs `REPACK (CONCURRENTLY)`; see the last point below.

What exists so far:

- a loadable library whose hooks detect when a table's storage is replaced
  and whether `REPACK (CONCURRENTLY)` did it;
- the writer: with the library loaded, `REPACK (CONCURRENTLY)` of a table
  that has data writes a record, (table, rewriting transaction ID), into the
  extension's table `pg_repack_safe_rewrites`, plus one for the table's TOAST
  relation. `REPACK (CONCURRENTLY)` is refused in a database where the
  extension is not created, and when its record table cannot be found or has
  been redefined;
- a placeholder reader check. The check that compares records with the
  reader's snapshot is not written, so with the library loaded **every query
  in the database is refused while the record table is non-empty**, that is,
  after any `REPACK (CONCURRENTLY)` of a table with data, until the table is
  emptied with `TRUNCATE pg_repack_safe_rewrites`. Queries are also refused
  if the extension exists but the table has been renamed, moved to another
  schema, or dropped. That second refusal is off while the server runs in
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
