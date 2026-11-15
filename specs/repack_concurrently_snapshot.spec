# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# REPACK (CONCURRENTLY) against a REPEATABLE READ transaction whose snapshot
# predates the repack and which has not yet read the repacked table.
#
# Stock PostgreSQL 19 documents this as not MVCC-safe (ref/repack.sgml,
# mvcc.sgml "Caveats"): REPACK does not wait for the reader, and after it
# commits the reader sees the table as empty. The expected output pins that
# wrong result: r_read returns count 0 for a table that holds 5 rows.
#
# The "touched" permutation is the control: a reader that has read the table
# holds ACCESS SHARE on it, so REPACK waits for it and the reader sees 5 rows.

setup
{
    CREATE TABLE accounts (id int PRIMARY KEY, balance int NOT NULL);
    INSERT INTO accounts SELECT g, 100 FROM generate_series(1, 5) g;
    CREATE TABLE audit_log (id int);
}

teardown
{
    DROP TABLE accounts, audit_log;
}

session reader
step r_begin    { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM audit_log; }
step r_touch    { SELECT count(*) FROM accounts; }
step r_read     { SELECT count(*), sum(balance) FROM accounts; }
step r_commit   { COMMIT; }

session repacker
step p_repack   { REPACK (CONCURRENTLY) accounts USING INDEX accounts_pkey; }
step p_check    { SELECT count(*), sum(balance) FROM accounts; }

# Wrong result: the reader's snapshot predates the repack and it holds no
# lock on accounts, so REPACK does not wait and the reader then sees 0 rows.
permutation r_begin p_repack r_read r_commit p_check

# Control: the reader has already read accounts, so REPACK waits for it.
permutation r_begin r_touch p_repack r_read r_commit p_check
