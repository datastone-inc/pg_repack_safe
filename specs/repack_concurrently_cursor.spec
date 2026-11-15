# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# REPACK (CONCURRENTLY) against an open cursor at the default READ COMMITTED
# isolation level.
#
# A cursor keeps the snapshot it was declared with. Here the cursor scans
# orders and looks each customer up through a STABLE PL/pgSQL function, which
# runs under the cursor's snapshot. PL/pgSQL is not inlined at plan time, so
# DECLARE takes no lock on customers; the first lock comes with the first
# function call. If REPACK (CONCURRENTLY) customers commits between DECLARE
# and the first FETCH, every lookup runs against the repacked table with the
# pre-repack snapshot and finds nothing. The expected output pins that wrong
# result: every name comes back NULL although each customer exists.
#
# Control: once a row has been fetched, the cursor holds ACCESS SHARE on
# customers and REPACK waits for the cursor's transaction to end.

setup
{
    CREATE TABLE customers (id int PRIMARY KEY, name text NOT NULL);
    INSERT INTO customers SELECT g, 'customer ' || g FROM generate_series(1, 3) g;
    CREATE TABLE orders (id int PRIMARY KEY, customer_id int NOT NULL);
    INSERT INTO orders SELECT g, g FROM generate_series(1, 3) g;
    CREATE FUNCTION customer_name(cid int) RETURNS text
        STABLE LANGUAGE plpgsql
        AS $$ BEGIN RETURN (SELECT name FROM customers WHERE id = cid); END $$;
}

teardown
{
    DROP FUNCTION customer_name(int);
    DROP TABLE orders, customers;
}

session reader
step c_declare  { BEGIN; DECLARE c CURSOR FOR SELECT o.id, customer_name(o.customer_id) AS name FROM orders o ORDER BY o.id; }
step c_fetch1   { FETCH 1 FROM c; }
step c_fetchall { FETCH ALL FROM c; }
step c_commit   { COMMIT; }

session repacker
step p_repack   { REPACK (CONCURRENTLY) customers USING INDEX customers_pkey; }

# Wrong result: every name is NULL.
permutation c_declare p_repack c_fetchall c_commit

# Control: the first FETCH locks customers, so REPACK waits for COMMIT.
permutation c_declare c_fetch1 p_repack c_fetchall c_commit
