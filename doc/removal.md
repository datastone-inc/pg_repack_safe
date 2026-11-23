# Removing the extension

Records protect snapshots taken before a `REPACK (CONCURRENTLY)`, on the
primary and on every standby. Dropping the extension removes that
protection, so it is refused while the record table holds any record,
deleted ones included.

The steps below are safe to stop and resume at any point. Each one ends
with a check; do not go on until it passes. If a check fails, the step
says what to do. Run every SQL command as a superuser.

1. **Stop running `REPACK (CONCURRENTLY)`.** Pause any job that runs it.
   Then check, on the primary, that none is in progress in any database:

   ```sql
   SELECT pid, datname, relid, command FROM pg_stat_progress_repack;
   ```

   Check: no rows. The view also shows plain `REPACK`, `CLUSTER` and
   `VACUUM FULL`; waiting for those too does no harm. If one slips in later,
   nothing breaks: its record survives step 2, step 3 does not reach empty,
   and step 4 is refused. If that happens, start again from step 1.

2. **List the databases that have the extension.** Steps 3 and 4 are
   done in every one of them; a database that is missed keeps the
   extension, and step 5 then leaves it unprotected without an error. On
   the primary:

   ```bash
   psql -XAt -c "SELECT datname FROM pg_database WHERE datallowconn" |
   while IFS= read -r db; do
       psql -XAt -d "$db" -c "SELECT format('database %I', current_database())
                                AS found
                              FROM pg_extension WHERE extname = 'pg_repack_safe'"
   done
   ```

   It prints one line for each database that has the extension, such as
   `database mydb`.

   Check: you have the list.

3. **Delete the records and vacuum them away.** In each database on the
   list, on the primary:

   ```sql
   SELECT repack_safe.prune_records();
   VACUUM (TRUNCATE) repack_safe.rewrites;
   SELECT pg_relation_size('repack_safe.rewrites') = 0 AS empty;
   ```

   Check: `empty` is `t`. `TRUNCATE` is spelled out so that a
   `vacuum_truncate = off` setting, for the server or the table, cannot
   keep the emptied pages.

   If `empty` is `f`, something still holds back the cleanup: `VACUUM`
   keeps a deleted record for as long as any snapshot could need it. This
   lists what holds it in this database:

   ```sql
   SELECT 'session' AS holder, pid::text AS id, backend_xmin AS xmin
   FROM pg_stat_activity
   WHERE datname = current_database() AND backend_xmin IS NOT NULL
     AND pid <> pg_backend_pid()
   UNION ALL
   SELECT 'prepared transaction', gid, transaction
   FROM pg_prepared_xacts WHERE database = current_database()
   UNION ALL
   SELECT 'replication slot', slot_name::text, xmin
   FROM pg_replication_slots WHERE xmin IS NOT NULL
   UNION ALL
   SELECT 'standby feedback', application_name, backend_xmin
   FROM pg_stat_replication WHERE backend_xmin IS NOT NULL;
   ```

   Wait for those to end (a standby with `hot_standby_feedback` reports
   its own oldest query), or end them, then run the `VACUUM` and the
   check again. A standby without `hot_standby_feedback` needs nothing:
   replaying the cleanup ends the standby snapshots that needed the
   records (see [Standby errors caused by pg_repack_safe](standbys.md)).

4. **Drop the extension.** In each database on the list, on the primary:

   ```sql
   DROP EXTENSION pg_repack_safe;
   SELECT count(*) = 0 AS dropped FROM pg_extension WHERE extname = 'pg_repack_safe';
   ```

   Check: `dropped` is `t` in every database on the list. Do not add
   `CASCADE`. If the drop is refused because the record table is not
   empty, go back to step 3. If it is refused because other objects depend
   on the extension (a view of the record table, say), look at them and
   drop them yourself.

   The drop leaves the empty schema `repack_safe` behind, since
   `CREATE EXTENSION` created it without making it part of the extension.
   Drop it too, unless you keep objects of your own there:

   ```sql
   DROP SCHEMA repack_safe;
   ```

5. **Decide whether to take `pg_repack_safe` out of
   `shared_preload_libraries`.** This step is optional. Only do this step 5
   after step 4 has dropped the extension in every database.

   > **Warning: never take the library out while the extension is still
   > installed, that is, never do step 5 before step 4.** That is not a
   > supported configuration, and PostgreSQL does not stop you. Without the
   > library, none of this extension's code runs, so nothing can object. The
   > database looks protected and is not: the record table and functions are
   > still there, and psql's `\dx` still lists `pg_repack_safe`. But
   >
   > - `REPACK (CONCURRENTLY)` succeeds without an error and writes no
   >   record (`t/009_not_preloaded.pl`);
   > - no query is checked, so old snapshots read the repacked table as
   >   empty, as in stock PostgreSQL.
   >
   > The catalog only shows that the extension is installed, not that it is
   > in force. NOTE HOWEVER that `\dx` does say how to check: the description of
   > `pg_repack_safe` reads "In force only where SELECT repack_safe.status()
   > returns true". Run that query. If it returns `f` (false) instead of `t`
   > (true), or fails with `could not access file`, the server is
   > misconfigured. (After step 4 the function is gone, so `function
   > repack_safe.status() does not exist` is expected there.) To fix it,
   > reinstall the package if the file is missing, put `pg_repack_safe` back
   > in `shared_preload_libraries`, restart, and start this procedure again
   > from step 1. Do not just drop the extension in that state: without the
   > library, `DROP EXTENSION` skips the check that refuses it while records
   > exist (`t/009_not_preloaded.pl`).

   At this point the extension is dropped and the library is still in
   `shared_preload_libraries`. Decide what you want `REPACK (CONCURRENTLY)`
   to do:

   - **Blocked:** leave the library in and skip to step 6. With the
     extension dropped, every `REPACK (CONCURRENTLY)` fails with
     `REPACK (CONCURRENTLY) requires the extension "pg_repack_safe" in this
     database` (`sql/storage_change.sql`). This is the safe choice. Running
     `CREATE EXTENSION` again in a database turns the guard back on there.
   - **Allowed again, unprotected:** take the library out, as below. The
     command then behaves as in stock PostgreSQL 19 and can make other
     sessions read the table as empty. To block it after all, set
     `max_repack_replication_slots = 0`
     (see [Without the extension](limits.md#without-the-extension)).

   Take it out on the primary first, then on each standby once it has
   replayed the drops. "Each standby" includes cascaded and delayed ones.
   A logical-replication subscriber is a separate server: if it has the
   extension, it needs this whole procedure of its own.

   A standby that is behind may still have to replay the extension's WAL
   record from a recent `REPACK (CONCURRENTLY)`, which it cannot do without
   the library: its replay would stop with "resource manager with ID 149
   not registered". And until it replays the `DROP EXTENSION`, the standby
   still has the extension, so without the library it would look protected
   and not be, as above. Past the drops, nothing the extension wrote is
   left for it to replay.

   On the primary, after the last `DROP EXTENSION`, note the WAL position:

   ```sql
   SELECT pg_current_wal_lsn();
   ```

   On each standby, wait until this returns `t`, with the position from
   the primary in place of `0/3000148`:

   ```sql
   SELECT pg_last_wal_replay_lsn() >= '0/3000148' AS replayed;
   ```

   On each server, find where the setting comes from:

   ```sql
   SELECT setting, source, sourcefile, sourceline
   FROM pg_settings WHERE name = 'shared_preload_libraries';
   ```

   Take `pg_repack_safe` out of the list there and keep the other libraries
   in it, spelled exactly as they are now. A misspelled library stops the
   server from starting.

   - `sourcefile` is `postgresql.conf` or a file it includes: edit that
     line.

     ```ini
     shared_preload_libraries = 'pg_stat_statements'   # was 'pg_stat_statements, pg_repack_safe'
     ```

   - `sourcefile` is `postgresql.auto.conf`, written by `ALTER SYSTEM`: set
     it again with `ALTER SYSTEM`, naming every library that stays, or reset
     it if `pg_repack_safe` was the only one. Do not edit that file by hand.

     ```sql
     ALTER SYSTEM SET shared_preload_libraries = 'pg_stat_statements';
     -- or, if pg_repack_safe was the only library:
     ALTER SYSTEM RESET shared_preload_libraries;
     ```

   - `source` is `command line`: change the server's start command or
     service file. On a managed service, change it in the provider's
     parameter settings.

   Then restart that server the way it is normally restarted (`pg_ctl
   restart -D "$PGDATA"`, `systemctl restart ...`, or the provider's
   console).

   Check: `SHOW shared_preload_libraries;` no longer lists
   `pg_repack_safe`, and still lists every other library.

6. **Uninstall the package** (`make uninstall` with the same `PG_CONFIG`
   as the install, or your package manager), host by host, only when both
   hold for that host:

   - the library is out of `shared_preload_libraries` there (step 5),
     since a server that preloads a library it cannot find does not
     start;
   - nothing kept will be restored on that host from before step 4: no
     base backup taken before the drops, and no archived WAL written
     before them.

   That WAL holds the extension's WAL records, and those backups still
   contain the extension. A server that restores from them (point-in-time
   recovery, a clone, a disaster-recovery host) needs the library
   installed and preloaded for the restore, like a standby; without it,
   recovery stops with "resource manager with ID 149 not registered".
   Keep the package on every host that may restore them until they expire.

Steps 1 to 4 remove the extension without exposing any snapshot or
breaking any standby, and need no backup to have expired.

A superuser can skip step 3 by setting
`pg_repack_safe.allow_unprotected_removal` in the session that runs the drop:

```sql
SET pg_repack_safe.allow_unprotected_removal = on;
DROP EXTENSION pg_repack_safe;
```

A snapshot taken before a recorded `REPACK (CONCURRENTLY)`, on the primary
or on any standby, then reads the repacked table as empty, so this is for
a server where no such snapshot can exist. Set it with `SET`, never in a
configuration file, so it ends with the session.
