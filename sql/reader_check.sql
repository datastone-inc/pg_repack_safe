/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file reader_check.sql
 * @brief The reader check in the ExecutorStart hook
 *
 * A query passes when the extension is absent from the database, or when its
 * record table has no blocks. A query is refused when the record table is not
 * empty, and when the extension exists but its record table cannot be found.
 * In that last state REPACK (CONCURRENTLY) is refused as well.
 */

LOAD 'pg_repack_safe';

-- Extension absent: no verdict is needed, the query passes.
SELECT 1 AS passes;

CREATE EXTENSION pg_repack_safe;

-- Empty record table: the query passes.
SELECT 1 AS passes;

-- A record makes the table non-empty: every query is refused.
INSERT INTO pg_repack_safe_rewrites VALUES ('pg_class'::regclass, '1');
SELECT 1 AS refused;
SELECT count(*) AS refused FROM pg_repack_safe_rewrites;

-- TRUNCATE is a utility statement and gives the table zero blocks again.
TRUNCATE pg_repack_safe_rewrites;
SELECT 1 AS passes;

-- With the record table in place, REPACK (CONCURRENTLY) runs.
CREATE TABLE rc_t (id int PRIMARY KEY);
REPACK (CONCURRENTLY) rc_t;

-- Record table renamed: it cannot be found, so the query is refused, and so
-- is REPACK (CONCURRENTLY), which would have nowhere to record the rewrite.
-- The MVCC-safe REPACK is a utility statement and still runs.
ALTER TABLE pg_repack_safe_rewrites RENAME TO rc_renamed;
SELECT 1 AS refused;
REPACK (CONCURRENTLY) rc_t;
REPACK (CONCURRENTLY true, VERBOSE false) rc_t;
REPACK rc_t;
ALTER TABLE rc_renamed RENAME TO pg_repack_safe_rewrites;
SELECT 1 AS passes;

-- Record table in another schema than the extension's: refused.
CREATE SCHEMA rc_other;
ALTER TABLE pg_repack_safe_rewrites SET SCHEMA rc_other;
SELECT 1 AS refused;
ALTER TABLE rc_other.pg_repack_safe_rewrites SET SCHEMA public;
SELECT 1 AS passes;

-- Record table detached from the extension and dropped: refused.
ALTER EXTENSION pg_repack_safe DROP TABLE pg_repack_safe_rewrites;
DROP TABLE pg_repack_safe_rewrites;
SELECT 1 AS refused;

-- Dropping the extension is a utility statement and ends the refusal.
DROP EXTENSION pg_repack_safe;
SELECT 1 AS passes;

-- Record table detached, then the extension dropped: only pg_extension
-- changes. The table is no longer a record table, so a row in it refuses
-- nothing.
CREATE EXTENSION pg_repack_safe;
SELECT 1 AS passes;
ALTER EXTENSION pg_repack_safe DROP TABLE pg_repack_safe_rewrites;
DROP EXTENSION pg_repack_safe;
INSERT INTO pg_repack_safe_rewrites VALUES ('pg_class'::regclass, '1');
SELECT 1 AS passes;
DROP TABLE pg_repack_safe_rewrites;

DROP SCHEMA rc_other;
DROP TABLE rc_t;
