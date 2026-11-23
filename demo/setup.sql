/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file setup.sql
 * @brief Tables for the demo: 100 customers with 10 orders each
 */

SET client_min_messages = warning;
DROP TABLE IF EXISTS orders, customers;

CREATE TABLE customers (id int PRIMARY KEY, name text NOT NULL);
INSERT INTO customers
SELECT g, 'customer ' || g FROM generate_series(1, 100) g;

CREATE TABLE orders (
    id int PRIMARY KEY,
    customer_id int NOT NULL,
    amount numeric(10, 2) NOT NULL,
    status text NOT NULL DEFAULT 'open'
);
INSERT INTO orders
SELECT g, (g - 1) % 100 + 1, 10.00 FROM generate_series(1, 1000) g;
