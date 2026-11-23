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

and restart. `session_preload_libraries` or `local_preload_libraries`
will not do: background workers never load them, so a
`REPACK (CONCURRENTLY)` run from one (a scheduled job, for instance) is
neither refused nor recorded. Each session that loads the library through
them gets a `WARNING` that names the setting.

Restart the standbys first: once `REPACK (CONCURRENTLY)` runs on the
primary, it writes a WAL record of the extension's own, and a standby
without the library fails on it and shuts down ("resource manager with
ID 149 not registered"). That is deliberate: such a standby could not check
its reads. Restart it with the library and it resumes replay.

On hot standbys that run queries, also set `hot_standby_feedback = on`,
ideally with a physical replication slot. It prevents a stock PostgreSQL
problem the extension does not cover: `VACUUM FULL`, `CLUSTER` and plain
`REPACK` on the primary can otherwise drop rows a standby query still
needs (README, "What pg_repack_safe does not guarantee").

## Enabling the Extension

In every database where `REPACK (CONCURRENTLY)` will run:

```sql
CREATE EXTENSION pg_repack_safe;
```

`REPACK (CONCURRENTLY)` is refused in a database without the extension,
and in a session where the library was loaded other than through
`shared_preload_libraries`.

The extension needs a superuser to create. Its table, `repack_safe.rewrites`,
and its functions are always in the schema `repack_safe`: PostgreSQL
reserves schema names that start with `pg_`, so the schema cannot carry the
extension's name. `CREATE EXTENSION` creates the schema and lets every role
look names up in it (`USAGE`), which `repack_safe.status()` needs; the table
and the other functions are for superusers. The extension cannot be created
in another schema or moved to one.

If a schema `repack_safe` already exists, the extension is created in it as
it is, so make sure beforehand that a superuser owns it. The schema can be
renamed afterwards (`ALTER SCHEMA repack_safe RENAME TO ...`) and the guard
keeps working; the commands in this documentation then need the new name.

PostgreSQL runs the extension's install script with `repack_safe` as the
`search_path`. In a database with DDL event triggers, their functions run
for each command of the script, so a function that names a table without
its schema fails there and `CREATE EXTENSION` fails with it
(`relation "..." does not exist`); name the table with its schema in the
function (`t/020_create_with_event_trigger.pl`).

Its settings:

| Setting | Default | Who | Meaning |
| --- | --- | --- | --- |
| `pg_repack_safe.precheck_snapshot_age` | `-1` (off) | superuser | refuse `REPACK (CONCURRENTLY)` while a session could later read the table with an older snapshot; a time (milliseconds without a unit, or e.g. `'10min'`), `0` for any age (README, "Optional pre-check") |
| `pg_repack_safe.allow_unprotected_removal` | `off` | superuser | let `DROP` and `TRUNCATE` remove the record table, its triggers or the extension, and let the record table be renamed or moved to another schema, while the record table holds records; old snapshots then read repacked tables as empty (README, "Removing the extension") |
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

For the same reason, rows of `repack_safe.rewrites` cannot be copied
from one database to another, even within one cluster, where relation OIDs
differ between databases: every `INSERT` and `COPY ... FROM` into the table
is refused, and only `REPACK (CONCURRENTLY)` writes records.

## Logical Replication

Keep `repack_safe.rewrites` out of every publication. As with a dump, a
record's relation OID and transaction ID mean something only in the cluster
that wrote them. On a subscriber the OID belongs to some other table, or to
none, and the transaction ID may not have been assigned there yet.

`CREATE PUBLICATION ... FOR ALL TABLES` includes the record table like any
other table. Exclude it when creating the publication:

```sql
CREATE PUBLICATION my_publication
    FOR ALL TABLES EXCEPT (TABLE repack_safe.rewrites);
```

or afterwards (the list replaces any earlier `EXCEPT` list):

```sql
ALTER PUBLICATION my_publication
    SET ALL TABLES EXCEPT (TABLE repack_safe.rewrites);
```

`FOR TABLES IN SCHEMA` has no `EXCEPT`: do not publish the schema
`repack_safe`.

If the table is published all the same (`t/016_publication.pl`):

- A `REPACK (CONCURRENTLY)` on the publisher does not disturb the
  subscription. The extension writes its record so that logical decoding
  passes over it, and nothing is sent.
- If the publisher's record table already holds records when the
  subscription is created, the initial copy of that one table reads them.
  A subscriber that has the extension refuses every insert into its record
  table, so the copy fails with `ERROR: cannot insert into the
  pg_repack_safe record table` and is retried; the other tables are copied
  and replicate.
- A record that arrives while the subscription is running, from any other
  source, is refused in the same way, in the apply worker. That refusal is
  an error, not a silent skip: the apply worker retries the same
  transaction for as long as it is refused, and meanwhile the subscription
  applies nothing, for any table.
- A `TRUNCATE` of the publisher's record table is refused as well while the
  subscriber's record table holds records of its own, which its old
  snapshots rely on: `ERROR: cannot truncate the pg_repack_safe record table
  while it holds records`. The subscription stops in the same way.

To recover, in this order:

1. On the publisher, exclude the table with the `ALTER PUBLICATION`
   statement above. This alone does not release a transaction that was
   already refused: the apply worker keeps failing on it.
2. On the subscriber, refresh the subscription, which drops the record
   table from it:

   ```sql
   ALTER SUBSCRIPTION my_subscription REFRESH PUBLICATION;
   ```

   The apply worker then passes over the record table's changes, the
   refused ones included, and continues with the other tables. Nothing is
   skipped but the publisher's records and its `TRUNCATE` of them. A failing initial copy ends as well.

A subscriber without the extension has no such table. Its apply worker
stops with PostgreSQL's own error, `logical replication target relation
"repack_safe.rewrites" does not exist`, so the exclusion is
needed there too.

## Other Extensions

The library chains its `object_access_hook`, `ProcessUtility_hook` and
`ExecutorStart_hook` to whatever was installed before it, in any load
order (`t/004_hook_chain.pl` with `pg_stat_statements`,
`t/015_oat_hook_chain.pl`). A statement it refuses in its
`ProcessUtility_hook` (`REPACK (CONCURRENTLY)` without the preload or the
extension, the optional pre-check, a refused `COPY ... TO`) is not passed on
to the hooks of libraries loaded before it: it must not run at all.
Libraries loaded before it also see `COPY partitioned_table TO` in the form
it is run in, `COPY (SELECT ...) TO` (README, "How it works").

A library listed after `pg_repack_safe` in `shared_preload_libraries` sees
every statement first, the refused ones included. So list `pg_repack_safe`
before an audit or statistics library that should record refused attempts:

```ini
shared_preload_libraries = 'pg_repack_safe, pg_stat_statements'
```

It registers a custom WAL resource manager with ID 149, reserved for
`pg_repack_safe` on the PostgreSQL wiki
([CustomWALResourceManagers](https://wiki.postgresql.org/wiki/CustomWALResourceManagers)).

## Verifying the Installation

On each server, and in each database with the extension:

```sql
SELECT repack_safe.status();
```

It must return `t`. With `f`, and a `WARNING` that says why, the server did
not load the library through `shared_preload_libraries`, and the extension
guards nothing there. If the library is in `session_preload_libraries` or
`local_preload_libraries`, the `WARNING` names that setting.
Nothing else reports this state: the extension is still listed by `\dx`,
and `REPACK (CONCURRENTLY)` runs without an error.

The primary and its standbys must run the same build of the library. On
each server, with or without the extension in the database:

```sql
SELECT module_name, version FROM pg_get_loaded_modules()
WHERE module_name = 'pg_repack_safe';
```

A server that preloads the library also writes `pg_repack_safe 1.0.0
loaded` to its log at each start, which shows afterwards which build it ran
and since when. The library's version (`1.0.0`) names the build; the
extension's version in `\dx` (`1.0`) names its SQL objects and changes only
when they do, so a new build of the library needs no `ALTER EXTENSION
UPDATE` unless its release notes say so.

To run the test suites:

```bash
make installcheck
```

This does not use the server that `PGHOST`/`PGPORT` point at: the test
suites start their own temporary servers from the installed binaries (see
[development.md](development.md)). It also installs four test-only modules,
`custom_scan_provider`, `oat_observer`, `test_record` and `toplevel_worker`,
into the PostgreSQL library directory;
`make install` does not. `test_record` can write arbitrary rows into a
record table, which the extension otherwise refuses, so do not run
`make installcheck` against the installation of a production server.

## Removing the Extension

See the README, "Removing the extension": dropping it is refused while its
record table holds records, deleted ones included, and the safe order is to
delete the records with `repack_safe.prune_records()`, vacuum the record
table until it is empty, and drop the extension. Removing the library from
`shared_preload_libraries` afterwards is optional: kept, it refuses
`REPACK (CONCURRENTLY)` in every database; removed, the command runs
unguarded. Uninstall the package only once no base backup or archived WAL
from before the drop is kept, since restoring those needs the library.
