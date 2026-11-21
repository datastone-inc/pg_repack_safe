# Installation Guide

## Prerequisites

- **PostgreSQL**: version 19, with `wal_level = replica` or higher
  (`REPACK (CONCURRENTLY)` requires it)
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

Install the same build on the primary and on every standby.

## Configuring the Servers

On the primary **and on every physical standby** (and any other server that
replays this primary's WAL), in `postgresql.conf`:

```ini
shared_preload_libraries = 'pg_repack_safe'   # add to any existing list
```

and restart. Standbys first: once `REPACK (CONCURRENTLY)` runs on the
primary, it writes a WAL record of the extension's own, and a standby
without the library fails on it and shuts down ("resource manager with
ID 128 not registered"). That is deliberate: such a standby could not check
its reads. Restart it with the library and it resumes replay.

On hot standbys that run queries, also set `hot_standby_feedback = on`,
ideally with a physical replication slot. It prevents a stock PostgreSQL
problem the extension does not cover: `VACUUM FULL`, `CLUSTER` and plain
`REPACK` on the primary can otherwise drop rows a standby query still
needs (README, "Known gaps").

## Enabling the Extension

In every database where `REPACK (CONCURRENTLY)` will run:

```sql
CREATE EXTENSION pg_repack_safe;
```

`REPACK (CONCURRENTLY)` is refused in a database without the extension,
and in a session where the library was loaded other than through
`shared_preload_libraries`.

The extension creates the table `pg_repack_safe_rewrites` in its schema.
It needs a superuser to create. Its settings:

| Setting | Default | Who | Meaning |
|---|---|---|---|
| `pg_repack_safe.precheck_snapshot_age` | `-1` (off) | any user | refuse `REPACK (CONCURRENTLY)` while a session could later read the table with an older snapshot (README, "Optional pre-check") |
| `pg_repack_safe.allow_unprotected_drop` | `off` | superuser | let `DROP` and `TRUNCATE` remove the record table, its triggers or the extension while records exist (README, "Removing the extension") |
| `pg_repack_safe.trace_storage_changes` | `off` | superuser | report each detected replacement of a table's storage as a NOTICE (diagnostics) |

## Upgrading PostgreSQL

`pg_upgrade` works with the library preloaded on both clusters
(`t/003_pg_upgrade.pl`). While a server runs in binary-upgrade mode,
queries pass when the record table is missing or lacks its triggers,
because `pg_restore` creates them in several steps;
`REPACK (CONCURRENTLY)` is refused in that state. Records are carried
over unchanged: `pg_upgrade` keeps relation OIDs and transaction IDs, so
each record still names the table and the rewrite it was written for. No
snapshot survives an upgrade, so the old records refuse nothing in the new
cluster.

## Dump and Restore

`pg_dump` does not dump the records, and a restore starts with an empty
record table (`t/001_repack_pg_dump.pl`). This is deliberate: the install
script does not mark the table with `pg_extension_config_dump()`. A record
is a relation OID and a transaction ID, and a restore keeps neither. The
restored tables get new OIDs, and the old transaction IDs name other
transactions in the new cluster, or none yet. Loaded there, an old record
would refuse reads of whichever table now has its OID. Nothing is lost by
leaving the records behind, because no snapshot survives a dump and
restore.

For the same reason, do not copy rows of `pg_repack_safe_rewrites` from one
database to another by hand, even within one cluster: relation OIDs differ
between databases.

## Other Extensions

The library chains its `object_access_hook`, `ProcessUtility_hook` and
`ExecutorStart_hook` to whatever was installed before it, in any load
order (`t/004_hook_chain.pl` with `pg_stat_statements`,
`t/015_oat_hook_chain.pl`). A statement it refuses in its
`ProcessUtility_hook` (`REPACK (CONCURRENTLY)` without the preload or the
extension, the optional pre-check, a refused `COPY ... TO`) is not passed on
to the hooks of libraries loaded before it: it must not run at all.

It registers a custom WAL resource manager with ID 128
(`RM_EXPERIMENTAL_ID`), which other extensions under development may also
use; two such libraries cannot be preloaded together. A permanent ID will
be registered before the first release.

## Verifying the Installation

```bash
make installcheck
```

This does not use the server that `PGHOST`/`PGPORT` point at: the test
suites start their own temporary servers from the installed binaries (see
[development.md](development.md)). It also installs a test-only module,
`oat_observer`, into the PostgreSQL library directory; `make install`
does not.

## Removing the Extension

See the README, "Removing the extension": dropping it is refused while it
holds records, and the safe order is to check for old snapshots on the
primary and every standby first, then drop it, then remove the library.
