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

DROP EXTENSION pg_repack_safe;

-- README.md, Optional pre-check
SET pg_repack_safe.precheck_snapshot_age = '10min';
SHOW pg_repack_safe.precheck_snapshot_age;
RESET pg_repack_safe.precheck_snapshot_age;

-- doc/installation.md, Logical Replication. wal_level is replica on the
-- test instance, which is enough to define a publication. The recovery
-- statement for a subscriber, ALTER SUBSCRIPTION ... REFRESH PUBLICATION,
-- needs two servers and runs in t/016_publication.pl.
CREATE EXTENSION pg_repack_safe;
CREATE PUBLICATION my_publication
    FOR ALL TABLES EXCEPT (TABLE pg_repack_safe_rewrites);
SELECT count(*) AS published FROM pg_publication_tables
WHERE pubname = 'my_publication' AND tablename = 'pg_repack_safe_rewrites';
ALTER PUBLICATION my_publication SET ALL TABLES;
SELECT count(*) AS published FROM pg_publication_tables
WHERE pubname = 'my_publication' AND tablename = 'pg_repack_safe_rewrites';
ALTER PUBLICATION my_publication
    SET ALL TABLES EXCEPT (TABLE pg_repack_safe_rewrites);
SELECT count(*) AS published FROM pg_publication_tables
WHERE pubname = 'my_publication' AND tablename = 'pg_repack_safe_rewrites';
DROP PUBLICATION my_publication;
DROP EXTENSION pg_repack_safe;

-- README.md, Removing the extension
CREATE EXTENSION pg_repack_safe;
CREATE TABLE de_t (id int PRIMARY KEY);
INSERT INTO de_t SELECT generate_series(1, 3);
REPACK (CONCURRENTLY) de_t;

-- Step 2.
SELECT pg_repack_safe_prune_records();

-- Step 3: no snapshot here needs the record.
VACUUM pg_repack_safe_rewrites;
SELECT pg_relation_size('pg_repack_safe_rewrites') = 0 AS empty;

-- Step 4.
DROP EXTENSION pg_repack_safe;

DROP TABLE de_t;
