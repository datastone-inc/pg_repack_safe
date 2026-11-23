/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file doc_examples.sql
 * @brief A test for every SQL example in README.md and the files in doc
 *
 * Each block is headed by the document and section its example comes from.
 */

-- README.md, Installation; doc/installation.md, Enabling the Extension
CREATE EXTENSION pg_repack_safe;

SELECT extname, extversion FROM pg_extension WHERE extname = 'pg_repack_safe';

-- README.md, Installation; doc/installation.md, Verifying the Installation
SELECT repack_safe.status();

-- The same two sections: the library's version, which varies from release
-- to release, so only its form is shown.
SELECT module_name, version ~ '^[0-9]+\.[0-9]+\.[0-9]+$' AS has_version
FROM pg_get_loaded_modules()
WHERE module_name = 'pg_repack_safe';

DROP EXTENSION pg_repack_safe;

-- doc/standbys.md, How to tell it was the record table. The database OID and
-- the filenode vary from run to run, so the lookups take them from the
-- catalogs; 1663 is pg_default. The log line they come from is checked in
-- t/017_standby_record_cleanup.pl.
CREATE EXTENSION pg_repack_safe;
SELECT oid AS db_oid FROM pg_database WHERE datname = current_database() \gset
SELECT pg_relation_filenode('repack_safe.rewrites') AS rec_filenode \gset
SELECT datname = current_database() AS is_this_database FROM pg_database WHERE oid = :db_oid;
SELECT pg_filenode_relation(1663, :rec_filenode);

-- The storage parameter that logs every autovacuum of the record table.
ALTER TABLE repack_safe.rewrites SET (log_autovacuum_min_duration = 0);
SELECT reloptions FROM pg_class WHERE oid = 'repack_safe.rewrites'::regclass;
DROP EXTENSION pg_repack_safe;

-- doc/installation.md, Optional pre-check
SET pg_repack_safe.precheck_snapshot_age = '10min';
SHOW pg_repack_safe.precheck_snapshot_age;
RESET pg_repack_safe.precheck_snapshot_age;

-- doc/installation.md, Logical Replication. wal_level is replica on the
-- test instance, which is enough to define a publication. The recovery
-- statement for a subscriber, ALTER SUBSCRIPTION ... REFRESH PUBLICATION,
-- needs two servers and runs in t/016_publication.pl.
CREATE EXTENSION pg_repack_safe;
CREATE PUBLICATION my_publication
    FOR ALL TABLES EXCEPT (TABLE repack_safe.rewrites);
SELECT count(*) AS published FROM pg_publication_tables
WHERE pubname = 'my_publication' AND schemaname = 'repack_safe'
  AND tablename = 'rewrites';
ALTER PUBLICATION my_publication SET ALL TABLES;
SELECT count(*) AS published FROM pg_publication_tables
WHERE pubname = 'my_publication' AND schemaname = 'repack_safe'
  AND tablename = 'rewrites';
ALTER PUBLICATION my_publication
    SET ALL TABLES EXCEPT (TABLE repack_safe.rewrites);
SELECT count(*) AS published FROM pg_publication_tables
WHERE pubname = 'my_publication' AND schemaname = 'repack_safe'
  AND tablename = 'rewrites';
DROP PUBLICATION my_publication;
DROP EXTENSION pg_repack_safe;

-- doc/removal.md
CREATE EXTENSION pg_repack_safe;
CREATE TABLE de_t (id int PRIMARY KEY);
INSERT INTO de_t SELECT generate_series(1, 3);
REPACK (CONCURRENTLY) de_t;

-- Step 1: nothing is being repacked.
SELECT pid, datname, relid, command FROM pg_stat_progress_repack;

-- Step 2: the query run in each database by the shell loop.
SELECT format('database %I', current_database()) AS found
FROM pg_extension WHERE extname = 'pg_repack_safe';

-- Step 3. vacuum_truncate is turned off for the table first, to show that
-- the TRUNCATE option overrides it.
SELECT repack_safe.prune_records();
ALTER TABLE repack_safe.rewrites SET (vacuum_truncate = off);
VACUUM (TRUNCATE) repack_safe.rewrites;
SELECT pg_relation_size('repack_safe.rewrites') = 0 AS empty;

-- Step 3, what holds back the cleanup: nothing here.
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

-- Step 4.
DROP EXTENSION pg_repack_safe;
SELECT count(*) = 0 AS dropped FROM pg_extension WHERE extname = 'pg_repack_safe';
DROP SCHEMA repack_safe;

-- Step 5. The WAL position varies from run to run, so only its presence is
-- shown here. This instance is not a standby, so the standby's check
-- returns NULL; on a standby it returns true once the drops are replayed.
SELECT pg_current_wal_lsn() IS NOT NULL AS has_position;
SELECT pg_last_wal_replay_lsn() >= '0/3000148' AS replayed;

-- Step 5, where the setting comes from. The temporary instance sets it in
-- postgresql.conf, whose path varies, so only the file's presence is shown.
SELECT setting, source, sourcefile IS NOT NULL AS has_sourcefile, sourceline IS NOT NULL AS has_sourceline
FROM pg_settings WHERE name = 'shared_preload_libraries';

-- Step 5, the ALTER SYSTEM forms. They write postgresql.auto.conf and take
-- effect only at a restart, which this temporary instance never has; the
-- RESET leaves the file as it was.
ALTER SYSTEM SET shared_preload_libraries = 'pg_stat_statements';
ALTER SYSTEM RESET shared_preload_libraries;
SHOW shared_preload_libraries;

-- README.md, Turning it off; doc/removal.md, the override: a drop with a
-- record left, in one session.
CREATE EXTENSION pg_repack_safe;
REPACK (CONCURRENTLY) de_t;
SELECT count(*) > 0 AS has_record FROM repack_safe.rewrites;
SET pg_repack_safe.allow_unprotected_removal = on;
DROP EXTENSION pg_repack_safe;
RESET pg_repack_safe.allow_unprotected_removal;

DROP TABLE de_t;
