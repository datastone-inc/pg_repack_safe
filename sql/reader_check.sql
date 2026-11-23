/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file reader_check.sql
 * @brief The reader check in the ExecutorStart hook
 *
 * A query passes when the extension is absent from the database, when its
 * record table has no blocks, and when every record for the relations it
 * reads has an XID its snapshot sees as committed, an aborted XID, or the
 * query's own transaction's XID. A record refuses a query only when its XID
 * committed and the query's snapshot does not see it, which takes a second
 * session: the isolation specs and t/005_reader_guard.pl cover that. A
 * query is also refused when the extension exists but its record table
 * cannot be found; in that state REPACK (CONCURRENTLY) is refused as well.
 *
 * Only REPACK (CONCURRENTLY) can insert into the record table
 * (protection.sql), and it records its own transaction. The records with
 * other transaction IDs that this test needs are written by
 * test_insert_record, from the test-only module test/modules/test_record,
 * the way the writer does: no trigger fires, and the record table's relcache
 * entry is invalidated.
 */

CREATE FUNCTION test_insert_record(record_table regclass, relid oid,
                                   rewrite_xid xid8)
RETURNS void
AS 'test_record'
LANGUAGE C STRICT;

-- This test truncates, renames, moves and drops the record table to reach
-- the reader's lookup failures. protection.sql tests the refusals of those
-- operations, so the superuser override is on here.
SET pg_repack_safe.allow_unprotected_removal = on;

-- Extension absent: no verdict is needed, the query passes.
SELECT 1 AS passes;

CREATE EXTENSION pg_repack_safe;

-- Empty record table: the query passes.
SELECT 1 AS passes;

CREATE TABLE rc_t (id int PRIMARY KEY);

-- Records this session's snapshots all see: queries pass.
-- The bootstrap XID precedes every snapshot.
SELECT test_insert_record('repack_safe.rewrites', 'pg_class'::regclass, '1');
SELECT count(*) > 0 AS passes FROM pg_class;
-- A transaction that committed before the snapshot was taken.
BEGIN;
SELECT pg_current_xact_id() AS committed_xid \gset
COMMIT;
SELECT test_insert_record('repack_safe.rewrites', 'rc_t'::regclass,
                          :'committed_xid');
SELECT count(*) AS passes FROM rc_t;
-- An aborted transaction.
BEGIN;
SELECT pg_current_xact_id() AS aborted_xid \gset
ROLLBACK;
SELECT test_insert_record('repack_safe.rewrites', 'rc_t'::regclass,
                          :'aborted_xid');
SELECT count(*) AS passes FROM rc_t;
-- The query's own transaction, whose XID was assigned after its snapshot.
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS passes FROM rc_t;
SELECT test_insert_record('repack_safe.rewrites', 'rc_t'::regclass,
                          pg_current_xact_id());
SELECT count(*) AS passes FROM rc_t;
COMMIT;
SELECT count(*) AS records FROM repack_safe.rewrites;

-- TRUNCATE is a utility statement and gives the table zero blocks again.
TRUNCATE repack_safe.rewrites;
SELECT 1 AS passes;

-- With the record table in place, REPACK (CONCURRENTLY) runs.
REPACK (CONCURRENTLY) rc_t;

-- Record table renamed: it cannot be found, so the query is refused, and so
-- is REPACK (CONCURRENTLY), which would have nowhere to record the rewrite.
-- The MVCC-safe REPACK is a utility statement and still runs.
ALTER TABLE repack_safe.rewrites RENAME TO rc_renamed;
SELECT 1 AS refused;
REPACK (CONCURRENTLY) rc_t;
REPACK (CONCURRENTLY true, VERBOSE false) rc_t;
REPACK rc_t;
ALTER TABLE repack_safe.rc_renamed RENAME TO rewrites;
SELECT 1 AS passes;

-- Record table in another schema than the extension's: refused.
CREATE SCHEMA rc_other;
ALTER TABLE repack_safe.rewrites SET SCHEMA rc_other;
SELECT 1 AS refused;
ALTER TABLE rc_other.rewrites SET SCHEMA repack_safe;
SELECT 1 AS passes;

-- Record table detached from the extension and dropped: refused.
ALTER EXTENSION pg_repack_safe DROP TABLE repack_safe.rewrites;
DROP TABLE repack_safe.rewrites;
SELECT 1 AS refused;

-- Dropping the extension is a utility statement and ends the refusal.
DROP EXTENSION pg_repack_safe;
SELECT 1 AS passes;

-- Record table detached, then the extension dropped: the table survives
-- and is no longer a record table, so a row in it refuses nothing. Its
-- triggers depend on the extension's trigger functions, so the drop needs
-- CASCADE and removes them, and an INSERT into the table is then an INSERT
-- into any table.
CREATE EXTENSION pg_repack_safe;
SELECT 1 AS passes;
ALTER EXTENSION pg_repack_safe DROP TABLE repack_safe.rewrites;
DROP EXTENSION pg_repack_safe CASCADE;
INSERT INTO repack_safe.rewrites VALUES ('pg_class'::regclass, '1');
SELECT 1 AS passes;
DROP TABLE repack_safe.rewrites;

DROP SCHEMA rc_other;
DROP TABLE rc_t;
DROP FUNCTION test_insert_record(regclass, oid, xid8);

RESET pg_repack_safe.allow_unprotected_removal;
