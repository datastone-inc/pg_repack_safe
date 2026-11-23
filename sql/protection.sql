/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file protection.sql
 * @brief Operations refused on the record table (fix-design.md, step 9)
 *
 * Records are the only protection old snapshots have, and only the library
 * deletes them (records_prune.sql). While the record table holds any row
 * version:
 * - DROP of the record table or the extension, by any route, and TRUNCATE
 *   of the record table, by any route (session_replication_role, CASCADE),
 *   are refused unless a superuser sets
 *   pg_repack_safe.allow_unprotected_removal;
 * - renaming the record table, moving it to another schema, and moving the
 *   extension to another schema without it, are refused under the same
 *   rule: the DROP refusals find the table by its name in the extension's
 *   schema;
 * - rewrites of the record table other than VACUUM FULL, CLUSTER and REPACK
 *   without CONCURRENTLY, which keep every record that is not deleted
 *   unchanged, are refused: REPACK (CONCURRENTLY) and rewriting ALTER TABLE
 *   (those three are refused too while a deleted record is in the table:
 *   records_prune.sql);
 * - UPDATE and DELETE of records are refused, by any route (MERGE, a
 *   writable CTE).
 * Changing when the record table's triggers fire is refused always, and
 * dropping them follows the DROP rule: they carry the UPDATE/DELETE,
 * TRUNCATE and INSERT refusals. A record table without both triggers,
 * firing ALWAYS, refuses every read and REPACK (CONCURRENTLY).
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
SELECT count(*) AS records FROM repack_safe.rewrites;

-- DROP and TRUNCATE: refused. (DROP TABLE of an extension member is
-- refused by PostgreSQL itself; the detached case below reaches this
-- extension's refusal.)
DROP TABLE repack_safe.rewrites;
TRUNCATE repack_safe.rewrites;
TRUNCATE pr_t, repack_safe.rewrites;
-- TRUNCATE is refused by the BEFORE TRUNCATE trigger, the one check that
-- every caller of the truncation passes: whatever session_replication_role
-- is (the trigger fires ALWAYS), and when the record table is only reached
-- by CASCADE, through a foreign key from it to the truncated table.
SET session_replication_role = replica;
TRUNCATE repack_safe.rewrites;
RESET session_replication_role;
CREATE TABLE pr_referenced (relid oid PRIMARY KEY);
INSERT INTO pr_referenced SELECT relid FROM repack_safe.rewrites;
ALTER TABLE repack_safe.rewrites
    ADD CONSTRAINT pr_fk FOREIGN KEY (relid) REFERENCES pr_referenced;
TRUNCATE pr_referenced CASCADE;
ALTER TABLE repack_safe.rewrites DROP CONSTRAINT pr_fk;
-- Constraints on the record table are not refused; the writer does not
-- enforce them. One could reach the records all the same: a foreign key
-- with ON DELETE CASCADE turns a DELETE on the referenced table into a
-- DELETE of records, which the record table's trigger refuses like any
-- other, and a CHECK constraint changes nothing.
ALTER TABLE repack_safe.rewrites
    ADD CONSTRAINT pr_fk FOREIGN KEY (relid) REFERENCES pr_referenced
    ON DELETE CASCADE;
DELETE FROM pr_referenced;
SELECT count(*) AS referenced FROM pr_referenced;
ALTER TABLE repack_safe.rewrites DROP CONSTRAINT pr_fk;
ALTER TABLE repack_safe.rewrites
    ADD CONSTRAINT pr_check CHECK (relid <> 0);
ALTER TABLE repack_safe.rewrites DROP CONSTRAINT pr_check;
DROP TABLE pr_referenced;
SELECT count(*) AS records FROM repack_safe.rewrites;
DROP EXTENSION pg_repack_safe;
DROP EXTENSION pg_repack_safe CASCADE;
-- Detached from the extension, then dropped, or the extension dropped:
-- refused.
ALTER EXTENSION pg_repack_safe DROP TABLE repack_safe.rewrites;
DROP TABLE repack_safe.rewrites;
DROP EXTENSION pg_repack_safe CASCADE;
ALTER EXTENSION pg_repack_safe ADD TABLE repack_safe.rewrites;

-- MVCC-safe rewrites, also database-wide, are allowed and keep every
-- record; REPACK (CONCURRENTLY) and ALTER TABLE rewrites are refused.
SELECT relid = 'pr_t'::regclass AS pr_t_record FROM repack_safe.rewrites;
VACUUM repack_safe.rewrites;
VACUUM FULL repack_safe.rewrites;
REPACK repack_safe.rewrites;
VACUUM FULL;
REPACK;
SELECT relid = 'pr_t'::regclass AS pr_t_record FROM repack_safe.rewrites;
ALTER TABLE repack_safe.rewrites SET UNLOGGED;
-- A rewriting column type change is refused as a column change, before it
-- reaches the rewrite.
ALTER TABLE repack_safe.rewrites
    ALTER COLUMN relid TYPE oid USING (relid::text)::oid;
-- PostgreSQL refuses this one itself (no identity index); were it to run,
-- the hook would refuse its swap like the ALTER TABLE rewrite above.
REPACK (CONCURRENTLY) repack_safe.rewrites;

-- UPDATE and DELETE: refused by any route.
UPDATE repack_safe.rewrites SET relid = 0;
DELETE FROM repack_safe.rewrites;
WITH d AS (DELETE FROM repack_safe.rewrites RETURNING 1)
SELECT count(*) FROM d;
MERGE INTO repack_safe.rewrites r
USING (VALUES (1)) v(x) ON true
WHEN MATCHED THEN DELETE;
SELECT count(*) AS records FROM repack_safe.rewrites;

-- INSERT: refused by any route, and whatever session_replication_role is
-- (a logical replication apply worker runs with replica).
INSERT INTO repack_safe.rewrites VALUES (0, '1');
INSERT INTO repack_safe.rewrites SELECT 0, '1'::xid8;
COPY repack_safe.rewrites FROM STDIN;
0	1
\.
MERGE INTO repack_safe.rewrites r
USING (VALUES (1)) v(x) ON false
WHEN NOT MATCHED THEN INSERT VALUES (0, '1');
INSERT INTO repack_safe.rewrites VALUES (0, '1') ON CONFLICT DO NOTHING;
WITH i AS (INSERT INTO repack_safe.rewrites VALUES (0, '1') RETURNING 1)
SELECT count(*) FROM i;
SET session_replication_role = replica;
INSERT INTO repack_safe.rewrites VALUES (0, '1');
INSERT INTO repack_safe.rewrites SELECT 0, '1'::xid8;
COPY repack_safe.rewrites FROM STDIN;
0	1
\.
MERGE INTO repack_safe.rewrites r
USING (VALUES (1)) v(x) ON false
WHEN NOT MATCHED THEN INSERT VALUES (0, '1');
RESET session_replication_role;
SELECT count(*) AS records FROM repack_safe.rewrites;
-- REPACK (CONCURRENTLY) still records.
REPACK (CONCURRENTLY) pr_t;
SELECT count(*) AS records, count(DISTINCT relid) AS tables
FROM repack_safe.rewrites;

-- The triggers: disabling, changing when they fire, or dropping them is
-- refused, whatever the spelling.
ALTER TABLE repack_safe.rewrites
    DISABLE TRIGGER rewrites_protected;
ALTER TABLE repack_safe.rewrites DISABLE TRIGGER ALL;
ALTER TABLE repack_safe.rewrites DISABLE TRIGGER USER;
ALTER TABLE repack_safe.rewrites
    ENABLE TRIGGER rewrites_protected;
ALTER TABLE repack_safe.rewrites
    ENABLE REPLICA TRIGGER rewrites_protected;
ALTER TABLE repack_safe.rewrites
    DISABLE TRIGGER rewrites_writer_only;
ALTER TABLE repack_safe.rewrites
    ENABLE TRIGGER rewrites_writer_only;
ALTER TABLE repack_safe.rewrites
    ENABLE REPLICA TRIGGER rewrites_writer_only;
DROP TRIGGER rewrites_protected ON repack_safe.rewrites;
DROP TRIGGER rewrites_writer_only ON repack_safe.rewrites;
-- Dropping a trigger function with CASCADE would drop its trigger: refused.
-- (PostgreSQL itself refuses to drop a member of the extension, so the
-- function is detached first.)
ALTER EXTENSION pg_repack_safe DROP FUNCTION repack_safe.records_protected();
DROP FUNCTION repack_safe.records_protected();
DROP FUNCTION repack_safe.records_protected() CASCADE;
ALTER EXTENSION pg_repack_safe ADD FUNCTION repack_safe.records_protected();
ALTER EXTENSION pg_repack_safe DROP FUNCTION repack_safe.records_writer_only();
DROP FUNCTION repack_safe.records_writer_only() CASCADE;
ALTER EXTENSION pg_repack_safe ADD FUNCTION repack_safe.records_writer_only();
-- Dropping the extension's schema with CASCADE would drop the record table
-- and its triggers: refused.
DROP SCHEMA repack_safe CASCADE;
SELECT count(*) AS triggers FROM pg_trigger
WHERE tgrelid = 'repack_safe.rewrites'::regclass;
-- Renaming a trigger changes nothing it does: allowed.
ALTER TRIGGER rewrites_protected ON repack_safe.rewrites
    RENAME TO pr_renamed;
ALTER TRIGGER pr_renamed ON repack_safe.rewrites
    RENAME TO rewrites_protected;
-- Re-enabling as ALWAYS is a no-op: allowed.
ALTER TABLE repack_safe.rewrites
    ENABLE ALWAYS TRIGGER rewrites_protected;

-- Indexes: refused in every spelling, and none is left behind. The writer
-- inserts records without maintaining indexes.
CREATE INDEX pr_idx ON repack_safe.rewrites (relid);
CREATE UNIQUE INDEX pr_idx ON repack_safe.rewrites (relid, rewrite_xid);
CREATE INDEX CONCURRENTLY pr_idx ON repack_safe.rewrites (relid);
ALTER TABLE repack_safe.rewrites ADD UNIQUE (relid, rewrite_xid);
ALTER TABLE repack_safe.rewrites ADD PRIMARY KEY (relid, rewrite_xid);
SELECT count(*) AS indexes FROM pg_index
WHERE indrelid = 'repack_safe.rewrites'::regclass;
-- An index on any other table is not affected.
CREATE INDEX pr_t_idx ON pr_t (id);
DROP INDEX pr_t_idx;

-- Columns: adding, dropping or altering one is refused. Accepted, each of
-- the first three would make every query in the database fail.
ALTER TABLE repack_safe.rewrites ADD COLUMN x text;
ALTER TABLE repack_safe.rewrites DROP COLUMN relid;
ALTER TABLE repack_safe.rewrites ALTER COLUMN relid TYPE regclass;
ALTER TABLE repack_safe.rewrites ALTER COLUMN relid SET DEFAULT 0;
ALTER TABLE repack_safe.rewrites RENAME COLUMN relid TO pr_renamed;
ALTER TABLE repack_safe.rewrites ALTER COLUMN relid DROP NOT NULL;
ALTER TABLE repack_safe.rewrites ALTER COLUMN relid SET STATISTICS 0;
SELECT attname, atttypid::regtype, atthasdef, attnotnull, attisdropped
FROM pg_attribute
WHERE attrelid = 'repack_safe.rewrites'::regclass AND attnum > 0
ORDER BY attnum;
SELECT count(*) AS passes FROM pr_t;
-- No column of the record table is an object some other object's drop
-- could take with it, so a drop of the whole table or of the extension
-- drops no column separately.
SELECT count(*) AS column_dependencies FROM pg_depend
WHERE classid = 'pg_class'::regclass
  AND objid = 'repack_safe.rewrites'::regclass AND objsubid <> 0;

-- The drop refusals find the record table by its name in the extension's
-- schema, so while it holds records it cannot be renamed or moved out of
-- that schema: DROP EXTENSION would then drop it unrecognized.
CREATE SCHEMA pr_m;
ALTER TABLE repack_safe.rewrites RENAME TO pr_renamed;
ALTER TABLE repack_safe.rewrites SET SCHEMA pr_m;
-- The extension's schema is fixed (pg_repack_safe.control): PostgreSQL
-- refuses to move the extension, with its record table or after the table
-- was detached from it.
ALTER EXTENSION pg_repack_safe SET SCHEMA pr_m;
ALTER EXTENSION pg_repack_safe DROP TABLE repack_safe.rewrites;
ALTER EXTENSION pg_repack_safe SET SCHEMA pr_m;
ALTER EXTENSION pg_repack_safe ADD TABLE repack_safe.rewrites;
DROP SCHEMA pr_m;
SELECT count(*) AS records FROM repack_safe.rewrites;
DROP EXTENSION pg_repack_safe;
SELECT count(*) AS passes FROM pr_t;

-- The override is for superusers only.
CREATE ROLE pr_user;
SET ROLE pr_user;
SET pg_repack_safe.allow_unprotected_removal = on;
-- So is the pre-check's setting: a role that could turn it off would undo
-- what the administrator configured.
SET pg_repack_safe.precheck_snapshot_age = -1;
RESET ROLE;
DROP ROLE pr_user;

-- With the override, DROP, TRUNCATE and renaming are allowed; ALTER TABLE
-- rewrites, UPDATE, DELETE, INSERT, the triggers, indexes and column changes
-- stay refused.
SET pg_repack_safe.allow_unprotected_removal = on;
ALTER TABLE repack_safe.rewrites RENAME TO pr_renamed;
ALTER TABLE repack_safe.pr_renamed RENAME TO rewrites;
ALTER TABLE repack_safe.rewrites SET UNLOGGED;
ALTER TABLE repack_safe.rewrites
    ALTER COLUMN relid TYPE oid USING (relid::text)::oid;
CREATE INDEX pr_idx ON repack_safe.rewrites (relid);
ALTER TABLE repack_safe.rewrites ADD UNIQUE (relid, rewrite_xid);
ALTER TABLE repack_safe.rewrites ADD COLUMN x text;
ALTER TABLE repack_safe.rewrites DROP COLUMN relid;
ALTER TABLE repack_safe.rewrites ALTER COLUMN relid TYPE regclass;
ALTER TABLE repack_safe.rewrites ALTER COLUMN relid SET DEFAULT 0;
ALTER TABLE repack_safe.rewrites RENAME COLUMN relid TO pr_renamed;
SELECT count(*) AS indexes FROM pg_index
WHERE indrelid = 'repack_safe.rewrites'::regclass;
SELECT count(*) AS passes FROM pr_t;
DELETE FROM repack_safe.rewrites;
INSERT INTO repack_safe.rewrites VALUES (0, '1');
COPY repack_safe.rewrites FROM STDIN;
0	1
\.
ALTER TABLE repack_safe.rewrites DISABLE TRIGGER ALL;
TRUNCATE repack_safe.rewrites;
INSERT INTO repack_safe.rewrites VALUES (0, '1');
SELECT count(*) AS records FROM repack_safe.rewrites;
-- The extension is dropped while its table holds a record again.
REPACK (CONCURRENTLY) pr_t;
SELECT count(*) AS records FROM repack_safe.rewrites;
DROP EXTENSION pg_repack_safe;
RESET pg_repack_safe.allow_unprotected_removal;

-- Without records, dropping a trigger is allowed, and then every read and
-- every REPACK (CONCURRENTLY) is refused until the extension is re-created:
-- the same state a backend without the library could leave behind.
CREATE EXTENSION pg_repack_safe;
DROP TRIGGER rewrites_protected ON repack_safe.rewrites;
SELECT count(*) AS refused FROM pr_t;
REPACK (CONCURRENTLY) pr_t;
DROP EXTENSION pg_repack_safe;
SELECT count(*) AS passes FROM pr_t;
-- The same without the trigger that refuses INSERT.
CREATE EXTENSION pg_repack_safe;
DROP TRIGGER rewrites_writer_only ON repack_safe.rewrites;
SELECT count(*) AS refused FROM pr_t;
REPACK (CONCURRENTLY) pr_t;
DROP EXTENSION pg_repack_safe;
SELECT count(*) AS passes FROM pr_t;

DROP TABLE pr_t;
