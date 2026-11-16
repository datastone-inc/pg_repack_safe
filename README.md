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

The extension is at the skeleton stage. **It does not yet prevent any wrong
result**: `REPACK (CONCURRENTLY)` behaves exactly as in stock PostgreSQL 19.

What exists so far:

- a loadable library whose hooks detect when a table's storage is replaced
  and whether `REPACK (CONCURRENTLY)` did it;
- an empty record table, `pg_repack_safe_rewrites`, that the guard will use.
  Nothing writes to it yet. With the library loaded, queries are refused if
  it is ever non-empty, because the check that would read it is not written.
  Queries are also refused if the extension exists but the table has been
  renamed, moved to another schema, or dropped. That second refusal is off
  while the server runs in binary-upgrade mode (`pg_upgrade`, or a server
  started by hand with `-b`), because `pg_upgrade` restores the extension
  before its table. `REPACK (CONCURRENTLY)` is refused whenever the extension
  exists and the table cannot be found, in that mode too.

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
