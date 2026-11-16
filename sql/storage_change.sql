/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file storage_change.sql
 * @brief Storage-replacement detection in the object access hook
 *
 * Each statement that replaces a table's storage must produce exactly one
 * NOTICE for that table, with the old storage's block count, and only
 * REPACK (CONCURRENTLY) may be classified as an unsafe rewriter. Statements
 * that alter pg_class without replacing storage must produce none.
 */

LOAD 'pg_repack_safe';
SET pg_repack_safe.trace_storage_changes = on;

CREATE TABLE sc_t (id int PRIMARY KEY, v text);
INSERT INTO sc_t SELECT g, 'row ' || g FROM generate_series(1, 3) g;
CREATE TABLE sc_empty (id int PRIMARY KEY);
-- Creating a materialized view populates it through a swap of its own
-- still-empty storage, which is detected like any other.
CREATE MATERIALIZED VIEW sc_mv AS SELECT g FROM generate_series(1, 3) g;

-- No storage change: no NOTICE.
ALTER TABLE sc_t SET (fillfactor = 90);
ALTER TABLE sc_t RENAME COLUMN v TO w;
ALTER TABLE sc_t ADD COLUMN extra int;

-- MVCC-safe rewrites: storage replaced, not an unsafe rewriter.
VACUUM FULL sc_t;
CLUSTER sc_t USING sc_t_pkey;
REPACK sc_t;
REPACK sc_t USING INDEX sc_t_pkey;

-- REPACK (CONCURRENTLY) in each option spelling: unsafe rewriter.
REPACK (CONCURRENTLY) sc_t USING INDEX sc_t_pkey;
REPACK (CONCURRENTLY true) sc_t;
REPACK (VERBOSE false, CONCURRENTLY on) sc_t;
REPACK (CONCURRENTLY false) sc_t;

-- An empty table: the old storage has zero blocks.
REPACK (CONCURRENTLY) sc_empty USING INDEX sc_empty_pkey;

-- Other storage replacements, detected but not classified as unsafe.
ALTER TABLE sc_t ALTER COLUMN extra TYPE bigint;
REFRESH MATERIALIZED VIEW sc_mv;

-- TRUNCATE raises no OAT_POST_ALTER, so it is not detected here.
TRUNCATE sc_t;

-- The classification does not leak past a failed statement.
REPACK (CONCURRENTLY) sc_missing;
VACUUM FULL sc_empty;

DROP MATERIALIZED VIEW sc_mv;
DROP TABLE sc_t, sc_empty;
