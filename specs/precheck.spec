# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The optional pre-check of REPACK (CONCURRENTLY)
# (doc/research/fix-design.md, step 11). Best-effort, not part of the
# guarantee: the reader guard protects every reader either way.
#
# With pg_repack_safe.precheck_snapshot_age >= 0, REPACK (CONCURRENTLY) is
# refused, at its start and again at the swap, while a client session in
# this database
# - holds a snapshot that outlives a statement (idle in transaction, or a
#   statement after the first of its transaction),
# - holds no lock on the table, so REPACK would not wait for it, and
# - has had its transaction open at least that long.
# Such a session would get a serialization failure if it read the table
# after the REPACK commits.

setup
{
    CREATE EXTENSION IF NOT EXISTS pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
    CREATE TABLE other (id int);
}

teardown
{
    DROP TABLE t, other;
}

session reader
step r_begin     { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step r_touch     { SELECT count(*) FROM t; }
step r_t         { SELECT count(*) FROM t; }
step r_commit    { COMMIT; }

session blocker
step b_lock      { BEGIN; SELECT count(*) FROM t; }
step b_commit    { COMMIT; }

session repacker
setup            { SET pg_repack_safe.precheck_snapshot_age = 0; }
step p_repack    { REPACK (CONCURRENTLY) t; }
step p_off       { SET pg_repack_safe.precheck_snapshot_age = -1; }
step p_hour      { SET pg_repack_safe.precheck_snapshot_age = '1h'; }

# At the start: the reader's snapshot has not touched t. Refused.
permutation r_begin p_repack r_commit
# The reader has touched t and holds a lock on it: not counted; REPACK
# waits for it at the swap instead and then succeeds.
permutation r_begin r_touch p_repack r_commit
# At the swap: REPACK waits for the blocker's lock; meanwhile the reader
# takes its snapshot without touching t. The blocker, a READ COMMITTED
# session between statements, holds no snapshot. Refused at the swap.
permutation b_lock p_repack r_begin b_commit r_commit
# The reader's transaction is younger than the threshold: REPACK runs, and
# the reader guard still refuses the reader.
permutation p_hour r_begin p_repack r_t r_commit
# The check is off: REPACK runs.
permutation p_off r_begin p_repack r_t r_commit
