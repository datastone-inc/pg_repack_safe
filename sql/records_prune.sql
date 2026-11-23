/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file records_prune.sql
 * @brief Deleting records: repack_safe.prune_records() and what a
 *        deleted record changes
 *
 * A record is deleted, never removed, by the extension: the deleted row
 * version stays in the table until VACUUM finds that no snapshot can need
 * it, and the reader loads every row version. This test covers what one
 * session can see:
 * - the function deletes every committed record older than its own
 *   transaction, and no other;
 * - a deleted record is still physically present, and is gone after VACUUM
 *   when no snapshot needs it;
 * - while the record table holds a deleted row version, VACUUM FULL and
 *   REPACK of it are refused, because they would drop the version without
 *   ending the standby snapshots that need it; after VACUUM they are
 *   allowed;
 * - pruned and vacuumed to zero blocks, the record table no longer stands
 *   in the way of DROP EXTENSION.
 * Readers with older snapshots: specs/record_prune.spec and
 * t/017_standby_record_cleanup.pl.
 */

CREATE EXTENSION pageinspect;
CREATE EXTENSION pg_repack_safe;
CREATE FUNCTION test_insert_record(record_table regclass, relid oid,
                                   rewrite_xid xid8)
RETURNS void
AS 'test_record'
LANGUAGE C STRICT;

-- Whether any row version, deleted or not, is physically in the record
-- table's first block.
CREATE FUNCTION rp_present() RETURNS boolean
LANGUAGE sql AS $$
    SELECT count(*) > 0 FROM heap_page_items(
        get_raw_page('repack_safe.rewrites', 0))
    WHERE lp_len > 0
$$;

-- Not for everyone.
SELECT has_function_privilege('public', 'repack_safe.prune_records()',
                              'EXECUTE') AS public_may_execute;

-- An empty record table: nothing to delete.
SELECT repack_safe.prune_records() AS deleted;

CREATE TABLE rp_t (id int PRIMARY KEY);
INSERT INTO rp_t SELECT generate_series(1, 5);
REPACK (CONCURRENTLY) rp_t;
REPACK (CONCURRENTLY) rp_t;
SELECT count(*) AS records, rp_present() AS present
FROM repack_safe.rewrites;

-- A record that names a transaction not older than the deleter's is left:
-- the deleter's transaction must follow the record's.
BEGIN;
SELECT test_insert_record('repack_safe.rewrites', 'rp_t'::regclass,
                          pg_current_xact_id());
SELECT repack_safe.prune_records() AS deleted;
SELECT count(*) AS records FROM repack_safe.rewrites;
ROLLBACK;
SELECT count(*) AS records FROM repack_safe.rewrites;

-- Every committed older record is deleted, whatever table it names, and
-- stays physically present.
SELECT repack_safe.prune_records() AS deleted;
SELECT count(*) AS records, rp_present() AS present
FROM repack_safe.rewrites;
SELECT count(*) AS readable FROM rp_t;
-- Nothing is left to delete.
SELECT repack_safe.prune_records() AS deleted;

-- An aborted prune deletes nothing.
REPACK (CONCURRENTLY) rp_t;
BEGIN;
SELECT repack_safe.prune_records() AS deleted;
ROLLBACK;
SELECT count(*) AS records FROM repack_safe.rewrites;
SELECT repack_safe.prune_records() AS deleted;

-- While a deleted row version is in the table, the rewrites that are
-- otherwise allowed are refused.
VACUUM FULL repack_safe.rewrites;
REPACK repack_safe.rewrites;
-- The database-wide forms stop at the record table with the same error.
VACUUM FULL;
REPACK;
SELECT rp_present() AS present;

-- VACUUM removes the deleted versions, no snapshot needing them, and the
-- table is back to zero blocks. The rewrites are allowed again.
VACUUM repack_safe.rewrites;
SELECT pg_relation_size('repack_safe.rewrites') AS bytes;
VACUUM FULL repack_safe.rewrites;
REPACK repack_safe.rewrites;
VACUUM FULL;
REPACK;

-- A live record does not refuse them either.
REPACK (CONCURRENTLY) rp_t;
VACUUM FULL repack_safe.rewrites;
SELECT count(*) AS records FROM repack_safe.rewrites;

-- Removing the extension: prune, VACUUM to zero blocks, then DROP EXTENSION
-- needs no override.
DROP EXTENSION pg_repack_safe;
SELECT repack_safe.prune_records() AS deleted;
VACUUM repack_safe.rewrites;
SELECT pg_relation_size('repack_safe.rewrites') AS bytes;
DROP EXTENSION pg_repack_safe;

DROP TABLE rp_t;
DROP FUNCTION rp_present();
DROP FUNCTION test_insert_record(regclass, oid, xid8);
DROP EXTENSION pageinspect;
