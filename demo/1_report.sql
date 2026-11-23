/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file 1_report.sql
 * @brief A report reads a table that was repacked after its snapshot
 */

-- A report runs in one transaction so that its figures agree with each
-- other. It reads customers first.
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS customers FROM customers;

-- Meanwhile another session repacks orders.
\! psql -X -q -c "REPACK (CONCURRENTLY) orders"

-- The report goes on to orders. There are 1000 of them, worth 10000.00.
SELECT count(*) AS orders, sum(amount) AS revenue FROM orders;
COMMIT;

-- The report runs again in a new transaction. With the extension this is
-- the retry the error asks for. On stock PostgreSQL nothing asks for it, and
-- the report above has already given its wrong figures.
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS customers FROM customers;
SELECT count(*) AS orders, sum(amount) AS revenue FROM orders;
COMMIT;
