/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file 2_update.sql
 * @brief An UPDATE in a transaction whose snapshot predates the repack
 */

-- A transaction looks up a customer, then cancels that customer's orders.
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT name FROM customers WHERE id = 7;

-- Meanwhile another session repacks orders.
\! psql -X -q -c "REPACK (CONCURRENTLY) orders"

-- Customer 7 has 10 open orders.
UPDATE orders SET status = 'cancelled' WHERE customer_id = 7;
COMMIT;

-- The orders the transaction believes it cancelled.
SELECT count(*) AS still_open FROM orders
WHERE customer_id = 7 AND status = 'open';
