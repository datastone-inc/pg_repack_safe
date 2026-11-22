/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file protection.sql
 * @brief Operations refused on the record table (fix-design.md, step 9)
 *
 * Records are the only protection old snapshots have, and they are never
 * deleted. While the record table holds any row version:
 * - DROP of the record table or the extension, by any route, and TRUNCATE
 *   of the record table are refused unless a superuser sets
 *   pg_repack_safe.allow_unprotected_drop;
 * - rewrites of the record table other than VACUUM FULL, CLUSTER and REPACK
 *   without CONCURRENTLY, which keep every record unchanged, are refused:
 *   REPACK (CONCURRENTLY) and rewriting ALTER TABLE;
 * - UPDATE and DELETE of records are refused, by any route (MERGE, a
 *   writable CTE).
 * Changing when the record table's triggers fire is refused always, and
 * dropping them follows the DROP rule: they carry the cache invalidation and
 * the UPDATE/DELETE and INSERT refusals. A record table without all three
 * triggers, firing ALWAYS, refuses every read and REPACK (CONCURRENTLY).
 * Refused always, with or without records and whatever the override says:
 * - INSERT into the record table, by any route (INSERT ... SELECT, COPY
 *   FROM, MERGE, a writable CTE) and whatever session_replication_role is:
 *   only REPACK (CONCURRENTLY) writes records. A row from anywhere else,
 *   such as another cluster's record arriving by logical replication
 *   (t/016_publication.pl), would refuse reads of whichever table has its
 *   OID here;
 * - creating an index on the record table, in any spelling (CREATE INDEX,
 *   a PRIMARY KEY or UNIQUE constraint): the writer inserts records without
 *   maintaining indexes, so an index would miss every later record;
 * - adding, dropping or altering a column of the record table (its type,
 *   default, name or any other property): with any other column list every
 *   query in the database would be refused.
 */

CREATE EXTENSION pg_repack_safe;

-- No record yet: dropping the extension is allowed.
DROP EXTENSION pg_repack_safe;
CREATE EXTENSION pg_repack_safe;

CREATE TABLE pr_t (id int PRIMARY KEY);
INSERT INTO pr_t SELECT generate_series(1, 5);
REPACK (CONCURRENTLY) pr_t;
SELECT count(*) AS records FROM pg_repack_safe_rewrites;

-- DROP and TRUNCATE: refused. (DROP TABLE of an extension member is
-- refused by PostgreSQL itself; the detached case below reaches this
-- extension's refusal.)
DROP TABLE pg_repack_safe_rewrites;
TRUNCATE pg_repack_safe_rewrites;
TRUNCATE pr_t, pg_repack_safe_rewrites;
DROP EXTENSION pg_repack_safe;
DROP EXTENSION pg_repack_safe CASCADE;
-- Detached from the extension, then dropped, or the extension dropped:
-- refused.
ALTER EXTENSION pg_repack_safe DROP TABLE pg_repack_safe_rewrites;
DROP TABLE pg_repack_safe_rewrites;
DROP EXTENSION pg_repack_safe CASCADE;
ALTER EXTENSION pg_repack_safe ADD TABLE pg_repack_safe_rewrites;

-- MVCC-safe rewrites, also database-wide, are allowed and keep every
-- record; REPACK (CONCURRENTLY) and ALTER TABLE rewrites are refused.
SELECT relid = 'pr_t'::regclass AS pr_t_record FROM pg_repack_safe_rewrites;
VACUUM pg_repack_safe_rewrites;
VACUUM FULL pg_repack_safe_rewrites;
REPACK pg_repack_safe_rewrites;
VACUUM FULL;
REPACK;
SELECT relid = 'pr_t'::regclass AS pr_t_record FROM pg_repack_safe_rewrites;
ALTER TABLE pg_repack_safe_rewrites SET UNLOGGED;
-- A rewriting column type change is refused as a column change, before it
-- reaches the rewrite.
ALTER TABLE pg_repack_safe_rewrites
    ALTER COLUMN relid TYPE oid USING (relid::text)::oid;
-- PostgreSQL refuses this one itself (no identity index); were it to run,
-- the hook would refuse its swap like the ALTER TABLE rewrite above.
REPACK (CONCURRENTLY) pg_repack_safe_rewrites;

-- UPDATE and DELETE: refused by any route.
UPDATE pg_repack_safe_rewrites SET relid = 0;
DELETE FROM pg_repack_safe_rewrites;
WITH d AS (DELETE FROM pg_repack_safe_rewrites RETURNING 1)
SELECT count(*) FROM d;
MERGE INTO pg_repack_safe_rewrites r
USING (VALUES (1)) v(x) ON true
WHEN MATCHED THEN DELETE;
SELECT count(*) AS records FROM pg_repack_safe_rewrites;

-- INSERT: refused by any route, and whatever session_replication_role is
-- (a logical replication apply worker runs with replica).
INSERT INTO pg_repack_safe_rewrites VALUES (0, '1');
INSERT INTO pg_repack_safe_rewrites SELECT 0, '1'::xid8;
COPY pg_repack_safe_rewrites FROM STDIN;
0	1
\.
MERGE INTO pg_repack_safe_rewrites r
USING (VALUES (1)) v(x) ON false
WHEN NOT MATCHED THEN INSERT VALUES (0, '1');
INSERT INTO pg_repack_safe_rewrites VALUES (0, '1') ON CONFLICT DO NOTHING;
WITH i AS (INSERT INTO pg_repack_safe_rewrites VALUES (0, '1') RETURNING 1)
SELECT count(*) FROM i;
SET session_replication_role = replica;
INSERT INTO pg_repack_safe_rewrites VALUES (0, '1');
INSERT INTO pg_repack_safe_rewrites SELECT 0, '1'::xid8;
COPY pg_repack_safe_rewrites FROM STDIN;
0	1
\.
MERGE INTO pg_repack_safe_rewrites r
USING (VALUES (1)) v(x) ON false
WHEN NOT MATCHED THEN INSERT VALUES (0, '1');
RESET session_replication_role;
SELECT count(*) AS records FROM pg_repack_safe_rewrites;
-- REPACK (CONCURRENTLY) still records.
REPACK (CONCURRENTLY) pr_t;
SELECT count(*) AS records, count(DISTINCT relid) AS tables
FROM pg_repack_safe_rewrites;

-- The triggers: disabling, changing when they fire, or dropping them is
-- refused, whatever the spelling.
ALTER TABLE pg_repack_safe_rewrites
    DISABLE TRIGGER pg_repack_safe_rewrites_changed;
ALTER TABLE pg_repack_safe_rewrites DISABLE TRIGGER ALL;
ALTER TABLE pg_repack_safe_rewrites DISABLE TRIGGER USER;
ALTER TABLE pg_repack_safe_rewrites
    ENABLE TRIGGER pg_repack_safe_rewrites_protected;
ALTER TABLE pg_repack_safe_rewrites
    ENABLE REPLICA TRIGGER pg_repack_safe_rewrites_changed;
ALTER TABLE pg_repack_safe_rewrites
    DISABLE TRIGGER pg_repack_safe_rewrites_writer_only;
ALTER TABLE pg_repack_safe_rewrites
    ENABLE TRIGGER pg_repack_safe_rewrites_writer_only;
ALTER TABLE pg_repack_safe_rewrites
    ENABLE REPLICA TRIGGER pg_repack_safe_rewrites_writer_only;
DROP TRIGGER pg_repack_safe_rewrites_changed ON pg_repack_safe_rewrites;
DROP TRIGGER pg_repack_safe_rewrites_protected ON pg_repack_safe_rewrites;
DROP TRIGGER pg_repack_safe_rewrites_writer_only ON pg_repack_safe_rewrites;
-- Dropping a trigger function with CASCADE would drop its trigger: refused.
-- (PostgreSQL itself refuses to drop a member of the extension, so the
-- function is detached first.)
ALTER EXTENSION pg_repack_safe DROP FUNCTION pg_repack_safe_records_protected();
DROP FUNCTION pg_repack_safe_records_protected();
DROP FUNCTION pg_repack_safe_records_protected() CASCADE;
ALTER EXTENSION pg_repack_safe ADD FUNCTION pg_repack_safe_records_protected();
ALTER EXTENSION pg_repack_safe DROP FUNCTION pg_repack_safe_records_writer_only();
DROP FUNCTION pg_repack_safe_records_writer_only() CASCADE;
ALTER EXTENSION pg_repack_safe ADD FUNCTION pg_repack_safe_records_writer_only();
-- Dropping the extension's schema with CASCADE would drop the extension:
-- refused.
DROP SCHEMA public CASCADE;
SELECT count(*) AS triggers FROM pg_trigger
WHERE tgrelid = 'pg_repack_safe_rewrites'::regclass;
-- Renaming a trigger changes nothing it does: allowed.
ALTER TRIGGER pg_repack_safe_rewrites_changed ON pg_repack_safe_rewrites
    RENAME TO pr_renamed;
ALTER TRIGGER pr_renamed ON pg_repack_safe_rewrites
    RENAME TO pg_repack_safe_rewrites_changed;
-- Re-enabling as ALWAYS is a no-op: allowed.
ALTER TABLE pg_repack_safe_rewrites
    ENABLE ALWAYS TRIGGER pg_repack_safe_rewrites_changed;

-- Indexes: refused in every spelling, and none is left behind. The writer
-- inserts records without maintaining indexes.
CREATE INDEX pr_idx ON pg_repack_safe_rewrites (relid);
CREATE UNIQUE INDEX pr_idx ON pg_repack_safe_rewrites (relid, rewrite_xid);
CREATE INDEX CONCURRENTLY pr_idx ON pg_repack_safe_rewrites (relid);
ALTER TABLE pg_repack_safe_rewrites ADD UNIQUE (relid, rewrite_xid);
ALTER TABLE pg_repack_safe_rewrites ADD PRIMARY KEY (relid, rewrite_xid);
SELECT count(*) AS indexes FROM pg_index
WHERE indrelid = 'pg_repack_safe_rewrites'::regclass;
-- An index on any other table is not affected.
CREATE INDEX pr_t_idx ON pr_t (id);
DROP INDEX pr_t_idx;

-- Columns: adding, dropping or altering one is refused. Accepted, each of
-- the first three would make every query in the database fail.
ALTER TABLE pg_repack_safe_rewrites ADD COLUMN x text;
ALTER TABLE pg_repack_safe_rewrites DROP COLUMN relid;
ALTER TABLE pg_repack_safe_rewrites ALTER COLUMN relid TYPE regclass;
ALTER TABLE pg_repack_safe_rewrites ALTER COLUMN relid SET DEFAULT 0;
ALTER TABLE pg_repack_safe_rewrites RENAME COLUMN relid TO pr_renamed;
ALTER TABLE pg_repack_safe_rewrites ALTER COLUMN relid DROP NOT NULL;
ALTER TABLE pg_repack_safe_rewrites ALTER COLUMN relid SET STATISTICS 0;
SELECT attname, atttypid::regtype, atthasdef, attnotnull, attisdropped
FROM pg_attribute
WHERE attrelid = 'pg_repack_safe_rewrites'::regclass AND attnum > 0
ORDER BY attnum;
SELECT count(*) AS passes FROM pr_t;
-- No column of the record table is an object some other object's drop
-- could take with it, so a drop of the whole table or of the extension
-- drops no column separately.
SELECT count(*) AS column_dependencies FROM pg_depend
WHERE classid = 'pg_class'::regclass
  AND objid = 'pg_repack_safe_rewrites'::regclass AND objsubid <> 0;

-- The extension can be moved to another schema; the record table is still
-- found there, with its records, and still protected.
CREATE SCHEMA pr_s;
ALTER EXTENSION pg_repack_safe SET SCHEMA pr_s;
SELECT count(*) AS records FROM pr_s.pg_repack_safe_rewrites;
CREATE INDEX pr_idx ON pr_s.pg_repack_safe_rewrites (relid);
ALTER TABLE pr_s.pg_repack_safe_rewrites ADD UNIQUE (relid, rewrite_xid);
ALTER TABLE pr_s.pg_repack_safe_rewrites ADD COLUMN x text;
ALTER TABLE pr_s.pg_repack_safe_rewrites DROP COLUMN relid;
ALTER TABLE pr_s.pg_repack_safe_rewrites ALTER COLUMN relid TYPE regclass;
DELETE FROM pr_s.pg_repack_safe_rewrites;
DROP SCHEMA pr_s CASCADE;
SELECT count(*) AS passes FROM pr_t;
ALTER EXTENSION pg_repack_safe SET SCHEMA public;
DROP SCHEMA pr_s;
SELECT count(*) AS records FROM pg_repack_safe_rewrites;

-- The override is for superusers only.
CREATE ROLE pr_user;
SET ROLE pr_user;
SET pg_repack_safe.allow_unprotected_drop = on;
RESET ROLE;
DROP ROLE pr_user;

-- With the override, DROP and TRUNCATE are allowed; ALTER TABLE rewrites,
-- UPDATE, DELETE, INSERT, the triggers, indexes and column changes stay
-- refused.
SET pg_repack_safe.allow_unprotected_drop = on;
ALTER TABLE pg_repack_safe_rewrites SET UNLOGGED;
ALTER TABLE pg_repack_safe_rewrites
    ALTER COLUMN relid TYPE oid USING (relid::text)::oid;
CREATE INDEX pr_idx ON pg_repack_safe_rewrites (relid);
ALTER TABLE pg_repack_safe_rewrites ADD UNIQUE (relid, rewrite_xid);
ALTER TABLE pg_repack_safe_rewrites ADD COLUMN x text;
ALTER TABLE pg_repack_safe_rewrites DROP COLUMN relid;
ALTER TABLE pg_repack_safe_rewrites ALTER COLUMN relid TYPE regclass;
ALTER TABLE pg_repack_safe_rewrites ALTER COLUMN relid SET DEFAULT 0;
ALTER TABLE pg_repack_safe_rewrites RENAME COLUMN relid TO pr_renamed;
SELECT count(*) AS indexes FROM pg_index
WHERE indrelid = 'pg_repack_safe_rewrites'::regclass;
SELECT count(*) AS passes FROM pr_t;
DELETE FROM pg_repack_safe_rewrites;
INSERT INTO pg_repack_safe_rewrites VALUES (0, '1');
COPY pg_repack_safe_rewrites FROM STDIN;
0	1
\.
ALTER TABLE pg_repack_safe_rewrites DISABLE TRIGGER ALL;
TRUNCATE pg_repack_safe_rewrites;
INSERT INTO pg_repack_safe_rewrites VALUES (0, '1');
SELECT count(*) AS records FROM pg_repack_safe_rewrites;
-- The extension is dropped while its table holds a record again.
REPACK (CONCURRENTLY) pr_t;
SELECT count(*) AS records FROM pg_repack_safe_rewrites;
DROP EXTENSION pg_repack_safe;
RESET pg_repack_safe.allow_unprotected_drop;

-- Without records, dropping a trigger is allowed, and then every read and
-- every REPACK (CONCURRENTLY) is refused until the extension is re-created:
-- the same state a backend without the library could leave behind.
CREATE EXTENSION pg_repack_safe;
DROP TRIGGER pg_repack_safe_rewrites_protected ON pg_repack_safe_rewrites;
SELECT count(*) AS refused FROM pr_t;
REPACK (CONCURRENTLY) pr_t;
DROP EXTENSION pg_repack_safe;
SELECT count(*) AS passes FROM pr_t;
-- The same without the trigger that refuses INSERT.
CREATE EXTENSION pg_repack_safe;
DROP TRIGGER pg_repack_safe_rewrites_writer_only ON pg_repack_safe_rewrites;
SELECT count(*) AS refused FROM pr_t;
REPACK (CONCURRENTLY) pr_t;
DROP EXTENSION pg_repack_safe;
SELECT count(*) AS passes FROM pr_t;

DROP TABLE pr_t;
