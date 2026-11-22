/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file 3_delete.sql
 * @brief A DELETE in a transaction whose snapshot predates the repack
 */

-- A transaction looks up a customer, then deletes that customer's orders
-- before deleting the customer.
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT name FROM customers WHERE id = 7;

-- Meanwhile another session repacks orders.
\! psql -X -q -c "REPACK (CONCURRENTLY) orders"

-- Customer 7 has 10 orders.
DELETE FROM orders WHERE customer_id = 7;
DELETE FROM customers WHERE id = 7;
COMMIT;

-- Customer 7, and the orders that name customer 7.
SELECT (SELECT count(*) FROM customers WHERE id = 7) AS customer,
       (SELECT count(*) FROM orders WHERE customer_id = 7) AS its_orders;
