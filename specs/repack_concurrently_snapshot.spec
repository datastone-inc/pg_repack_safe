# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# REPACK (CONCURRENTLY) against a REPEATABLE READ transaction whose snapshot
# predates the repack and which has not yet read the repacked table.
#
# Stock PostgreSQL 19 documents this as not MVCC-safe (ref/repack.sgml,
# mvcc.sgml "Caveats"): REPACK does not wait for the reader, and after it
# commits the reader sees the table as empty (count 0 for a table that holds
# 5 rows). With pg_repack_safe the expected output is an error at r_read
# instead: the reader's snapshot does not see the REPACK as committed.
# isolationtester prints only the primary message, so the SQLSTATE (40001) is
# not pinned here.
#
# The "touched" permutation is the control: a reader that has read the table
# holds ACCESS SHARE on it, so REPACK waits for it and the reader sees 5 rows.
#
# The extension is created once and never dropped: the instance is temporary,
# and dropping it while it holds records is refused (sql/protection.sql).

setup
{
    CREATE EXTENSION IF NOT EXISTS pg_repack_safe;
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

# The reader's snapshot predates the repack and it holds no lock on accounts,
# so REPACK does not wait. Stock: the reader sees 0 rows. Expected: r_read
# errors.
permutation r_begin p_repack r_read r_commit p_check

# Control: the reader has already read accounts, so REPACK waits for it.
permutation r_begin r_touch p_repack r_read r_commit p_check
