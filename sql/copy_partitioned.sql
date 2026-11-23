/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file copy_partitioned.sql
 * @brief COPY of a partitioned table gives what stock PostgreSQL 19 gives
 *
 * The library runs COPY partitioned_table TO as COPY (SELECT ...) TO, so
 * that the reader check covers exactly the partitions that are scanned
 * (specs/copy_attach_race.spec). Everything here is stock PostgreSQL 19's
 * output and errors for the relation form, which the query form must keep:
 * - the default column list leaves out dropped and generated columns;
 * - a partition with another physical column order, and a partition that
 *   is itself partitioned, give rows in the parent's column order;
 * - a column list is validated as for a relation: a generated, repeated or
 *   unknown column is an error;
 * - privileges are those of the relation, also column by column;
 * - a table with row-level security is left to PostgreSQL, which turns it
 *   into a query itself; that query is SELECT *, generated columns
 *   included.
 * One difference is accepted and documented in the README: a partition that
 * is a foreign table is read, where the relation form raises an error.
 */

CREATE TABLE cp (id int, gone int, a text,
                 g int GENERATED ALWAYS AS (id * 2) STORED,
                 v int GENERATED ALWAYS AS (id + 1) VIRTUAL,
                 b numeric) PARTITION BY RANGE (id);
CREATE TABLE cp1 PARTITION OF cp FOR VALUES FROM (0) TO (10);
CREATE TABLE cp2 (b numeric, junk int, a text, id int, gone int,
                  g int GENERATED ALWAYS AS (id * 2) STORED,
                  v int GENERATED ALWAYS AS (id + 1) VIRTUAL);
ALTER TABLE cp2 DROP COLUMN junk;
ALTER TABLE cp ATTACH PARTITION cp2 FOR VALUES FROM (10) TO (20);
CREATE TABLE cp3 PARTITION OF cp FOR VALUES FROM (20) TO (30)
    PARTITION BY RANGE (id);
CREATE TABLE cp3a PARTITION OF cp3 FOR VALUES FROM (20) TO (30);
INSERT INTO cp (id, gone, a, b)
SELECT g, g, 'row ' || g, g / 4.0 FROM generate_series(8, 21) g;
ALTER TABLE cp DROP COLUMN gone;

-- Default column list, with and without options.
COPY cp TO STDOUT;
COPY cp TO STDOUT WITH (FORMAT csv, HEADER);
COPY public.cp TO STDOUT WITH (FORMAT csv, FORCE_QUOTE (a), NULL 'none');

-- A column list, in another order than the table's.
COPY cp (b, id) TO STDOUT;

-- Column lists the relation form refuses.
COPY cp (id, g) TO STDOUT;
COPY cp (id, v) TO STDOUT;
COPY cp (id, id) TO STDOUT;
COPY cp (id, missing) TO STDOUT;
COPY cp (id, gone) TO STDOUT;

-- A partition that is itself partitioned, and a leaf, named directly.
COPY cp3 TO STDOUT;
COPY cp2 TO STDOUT;

-- Privileges are the relation's.
CREATE ROLE cp_reader;
SET ROLE cp_reader;
COPY cp TO STDOUT;
RESET ROLE;
GRANT SELECT (id) ON cp TO cp_reader;
SET ROLE cp_reader;
COPY cp (id) TO STDOUT;
COPY cp TO STDOUT;
COPY cp (id, a) TO STDOUT;
RESET ROLE;

-- Row-level security: PostgreSQL's own query form.
GRANT SELECT ON cp TO cp_reader;
ALTER TABLE cp ENABLE ROW LEVEL SECURITY;
CREATE POLICY cp_some ON cp USING (id < 10);
SET ROLE cp_reader;
COPY cp TO STDOUT;
COPY cp (b, id) TO STDOUT;
SET row_security = off;
COPY cp TO STDOUT;
RESET row_security;
RESET ROLE;

-- A partitioned table without partitions.
CREATE TABLE cp_empty (id int) PARTITION BY RANGE (id);
COPY cp_empty TO STDOUT;

-- Inside a function, whose cached statement tree is read-only and is run
-- twice. PL/pgSQL cannot COPY to the client, so the rows go to a file.
CREATE FUNCTION cp_copy() RETURNS bigint LANGUAGE plpgsql
AS $$
DECLARE
    n bigint;
BEGIN
    COPY cp (id) TO '/dev/null';
    GET DIAGNOSTICS n = ROW_COUNT;
    RETURN n;
END
$$;
SELECT cp_copy();
SELECT cp_copy();

DROP FUNCTION cp_copy();
DROP TABLE cp, cp_empty;
DROP ROLE cp_reader;
