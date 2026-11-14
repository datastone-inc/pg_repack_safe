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

The extension is at the skeleton stage: it installs and creates, and defines
no objects yet.

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
