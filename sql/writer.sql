/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file writer.sql
 * @brief The writer: unsafe-rewrite records written at the swap
 *
 * REPACK (CONCURRENTLY) of a table whose old storage has blocks writes one
 * record for the table and one for the TOAST relation the table has after
 * the swap, both with the rewriter's transaction ID. Nothing else writes a
 * record: not an empty table, not an MVCC-safe rewrite, not a failed REPACK.
 * Having written its records, the rewrite deletes every older one, so the
 * record table stays small however often tables are repacked.
 * The transient heap is told apart from the user's table by meaning: the
 * storage the swap gives it was created in the rewriter's transaction. A
 * user table named like the transient heap is still recorded. The old
 * storage's blocks are what count, so a table whose rows were all deleted,
 * and which an older snapshot could still read, is recorded. Without the
 * extension in the database, REPACK (CONCURRENTLY) is refused, since it
 * would have nowhere to record the rewrite.
 */

CREATE TABLE wr_t (id int PRIMARY KEY, v text);
INSERT INTO wr_t SELECT g, repeat('x', 3000) || g FROM generate_series(1, 3) g;
CREATE TABLE wr_empty (id int PRIMARY KEY);
-- Rows deleted but not vacuumed: no row is live, the old storage has blocks.
CREATE TABLE wr_deleted (id int PRIMARY KEY);
INSERT INTO wr_deleted SELECT generate_series(1, 3);
DELETE FROM wr_deleted;
-- A user table with the name the transient heap of a REPACK would carry.
CREATE TABLE public.pg_temp_1 (id int PRIMARY KEY);
INSERT INTO public.pg_temp_1 VALUES (1), (2);

-- Extension absent: refused, in every option spelling.
REPACK (CONCURRENTLY) wr_t;
REPACK (CONCURRENTLY true, VERBOSE false) wr_t USING INDEX wr_t_pkey;

CREATE EXTENSION pg_repack_safe;
CREATE EXTENSION pageinspect;

-- MVCC-safe rewrites: no record.
VACUUM FULL wr_t;
CLUSTER wr_t USING wr_t_pkey;
REPACK wr_t;
REPACK (CONCURRENTLY false) wr_t;
SELECT count(*) AS records FROM repack_safe.rewrites;

-- Empty table: the old storage has no blocks, no record.
REPACK (CONCURRENTLY) wr_empty;
SELECT count(*) AS records FROM repack_safe.rewrites;

-- Failed REPACK (CONCURRENTLY): no record.
REPACK (CONCURRENTLY) wr_missing;
REPACK (CONCURRENTLY) wr_t USING INDEX wr_missing_idx;
SELECT count(*) AS records FROM repack_safe.rewrites;

-- Unsafe rewrites of non-empty tables: recorded. Each record names the
-- table or its post-swap TOAST relation, and carries the full XID that wrote
-- it, whose 32-bit form also wrote every row of the repacked table. Each
-- REPACK (CONCURRENTLY) also deletes the records older than its own, so only
-- its own are visible after it.
CREATE VIEW wr_records AS
SELECT CASE r.relid
           WHEN 'wr_t'::regclass THEN 'wr_t'
           WHEN c.reltoastrelid THEN 'wr_t TOAST'
           WHEN 'public.pg_temp_1'::regclass THEN 'pg_temp_1'
           WHEN 'wr_deleted'::regclass THEN 'wr_deleted'
       END AS recorded,
       r.rewrite_xid::xid = r.xmin AS by_rewriter
FROM repack_safe.rewrites r, pg_class c
WHERE c.oid = 'wr_t'::regclass;

REPACK (CONCURRENTLY) wr_t;
SELECT * FROM wr_records ORDER BY recorded;
SELECT count(*) AS rows,
       bool_and(t.xmin = r.rewrite_xid::xid) AS rows_by_rewriter
FROM wr_t t, repack_safe.rewrites r
WHERE r.relid = 'wr_t'::regclass;

-- With ANALYZE the statement commits after the swap and analyzes in a new
-- transaction; the record is written, and the older ones deleted, at the
-- swap all the same.
REPACK (CONCURRENTLY, ANALYZE) public.pg_temp_1;
SELECT * FROM wr_records ORDER BY recorded;
SELECT bool_and(t.xmin = r.rewrite_xid::xid) AS rows_by_rewriter
FROM public.pg_temp_1 t, repack_safe.rewrites r
WHERE r.relid = 'public.pg_temp_1'::regclass;

REPACK (CONCURRENTLY) wr_deleted;
SELECT * FROM wr_records ORDER BY recorded;

-- The record table does not grow with the number of repacks. A table with a
-- TOAST relation gets a new one at every REPACK (CONCURRENTLY), and the
-- records of the dropped ones are deleted with the rest: after any number of
-- repacks the visible records are the last repack's two, both naming
-- relations that exist, and after VACUUM, with no snapshot needing the
-- deleted ones, they are all the table physically holds.
REPACK (CONCURRENTLY) wr_t;
REPACK (CONCURRENTLY) wr_t;
REPACK (CONCURRENTLY) wr_t;
REPACK (CONCURRENTLY) wr_t;
SELECT count(*) AS records, count(c.oid) AS naming_a_relation
FROM repack_safe.rewrites r LEFT JOIN pg_class c ON c.oid = r.relid;
SELECT * FROM wr_records ORDER BY recorded;
VACUUM repack_safe.rewrites;
SELECT count(*) AS row_versions
FROM heap_page_items(get_raw_page('repack_safe.rewrites', 0))
WHERE lp_len > 0;
SELECT pg_relation_size('repack_safe.rewrites') /
       current_setting('block_size')::int AS blocks;

-- wr_deleted was recorded on its old storage's blocks: its new storage is
-- empty.
SELECT pg_relation_size('wr_deleted') AS new_size;

-- The record table is a heap: the reader scans it with SnapshotAny.
SELECT a.amname
FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.oid = 'repack_safe.rewrites'::regclass;

-- The REPACKs above left records, so removing the extension takes the
-- superuser override (protection.sql tests the refusal).
SET pg_repack_safe.allow_unprotected_removal = on;
DROP EXTENSION pg_repack_safe;
RESET pg_repack_safe.allow_unprotected_removal;
DROP VIEW wr_records;
DROP TABLE wr_t, wr_empty, wr_deleted, public.pg_temp_1;
DROP EXTENSION pageinspect;
