# Installation Guide

## Prerequisites

- **PostgreSQL**: version 19
- **Build tools**: GCC or Clang with C99 support, and `make`
- **PostgreSQL development headers** (`pg_config --includedir-server` must
  resolve):
  - Debian/Ubuntu: `postgresql-server-dev-19`
  - Red Hat/CentOS: `postgresql19-devel`
  - Source build: installed with the server

```bash
pg_config --version      # PostgreSQL 19.x
```

## Building from Source

```bash
git clone https://github.com/datastone-inc/pg_repack_safe.git
cd pg_repack_safe
make
sudo make install        # omit sudo when the PostgreSQL install is yours
```

To build against a PostgreSQL that is not first on `PATH`, name its
`pg_config`:

```bash
make PG_CONFIG=/path/to/bin/pg_config
make install PG_CONFIG=/path/to/bin/pg_config
```

## Enabling the Extension

```sql
CREATE EXTENSION pg_repack_safe;
```

## Verifying the Installation

```bash
make installcheck        # runs the regression tests against the running server
```
