# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Edge cases of the reader guard (doc/research/fix-design.md, "Interleavings
# that would prove each claim", and open question 7).
#
# The reader takes a REPEATABLE READ snapshot without touching the tables
# below. Each permutation then changes them from other sessions and reads.
#
# No error expected:
# - VACUUM FULL or GRANT alone (interleavings 7, 9): MVCC-safe, no record;
# - a table created after the snapshot (13): no record, correct empty result;
# - an empty table repacked (14): zero blocks, no record;
# - a record for another table: refuses nothing.
# Error expected:
# - the reader's own GRANT on the table after the REPACK (11);
# - VACUUM FULL after the REPACK (12): the record outlives a safe rewrite;
# - every row deleted after the snapshot, then REPACK (15): a real wrong
#   result on stock PostgreSQL, so the zero-blocks exemption is not too wide;
# - reads through a view, an inlined set-returning SQL function, a parallel
#   plan, and the parent of a repacked partition (open question 7): each
#   puts the repacked relation in the range table the check reads. Parallel
#   workers are launched on the first ExecGather call (nodeGather.c, ExecGather),
#   after the leader's ExecutorStart has run the check.
# Accepted spurious errors (fix-design.md section 6b), pinned:
# - rows inserted into an empty table after the snapshot, then REPACK (16):
#   stock PostgreSQL returns a correct 0;
# - a table created after the snapshot and then repacked: same cause.
#
# Stock PostgreSQL 19, same interleavings without the library: every
# permutation expected to error above returns 0 rows for a table that holds
# 5; both accepted spurious errors return a correct 0; the others return what
# they return here.

setup
{
    CREATE EXTENSION IF NOT EXISTS pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY, v int NOT NULL);
    INSERT INTO t SELECT g, g FROM generate_series(1, 5) g;
    CREATE TABLE e (id int PRIMARY KEY);
    CREATE TABLE other (id int);
    CREATE VIEW t_view AS SELECT * FROM t;
    CREATE FUNCTION t_rows() RETURNS SETOF t
        STABLE LANGUAGE sql AS 'SELECT * FROM t';
    CREATE TABLE p (id int PRIMARY KEY) PARTITION BY RANGE (id);
    CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (1) TO (100);
    INSERT INTO p SELECT generate_series(1, 5);
}

teardown
{
    DROP TABLE IF EXISTS n;
    DROP TABLE p;
    DROP FUNCTION t_rows();
    DROP VIEW t_view;
    DROP TABLE t, e, other;
}

session reader
step r_begin     { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step r_t         { SELECT count(*), sum(v) FROM t; }
step r_e         { SELECT count(*) FROM e; }
step r_n         { SELECT count(*) FROM n; }
step r_view      { SELECT count(*) FROM t_view; }
step r_srf       { SELECT count(*) FROM t_rows(); }
step r_parallel  { SET LOCAL debug_parallel_query = on; SELECT count(*) FROM t; }
step r_parent    { SELECT count(*) FROM p; }
step r_grant     { GRANT SELECT ON t TO PUBLIC; }
step r_commit    { COMMIT; }

session repacker
step p_repack    { REPACK (CONCURRENTLY) t; }
step p_repack_e  { REPACK (CONCURRENTLY) e; }
step p_repack_n  { REPACK (CONCURRENTLY) n; }
step p_repack_p1 { REPACK (CONCURRENTLY) p1; }
step p_vacfull   { VACUUM FULL t; }
step p_grant     { GRANT SELECT ON t TO PUBLIC; }

session other
step o_delete    { DELETE FROM t; }
step o_insert_e  { INSERT INTO e SELECT generate_series(1, 3); }
step o_create_n  { CREATE TABLE n (id int PRIMARY KEY); INSERT INTO n VALUES (1); }

# 7: VACUUM FULL alone: every row, no error.
permutation r_begin p_vacfull r_t r_commit
# 9: GRANT alone: every row, no error.
permutation r_begin p_grant r_t r_commit
# 13: table created after the snapshot: 0 rows, no error.
permutation r_begin o_create_n r_n r_commit
# 14: empty table repacked: 0 rows, no error.
permutation r_begin p_repack_e r_e r_commit
# A record for another table: every row of t, no error.
permutation r_begin p_repack_p1 r_t r_commit

# 11: the reader's own DDL on t after the REPACK: error.
permutation r_begin p_repack r_grant r_t r_commit
# 12: VACUUM FULL after the REPACK: error.
permutation r_begin p_repack p_vacfull r_t r_commit
# 15: every row deleted after the snapshot, then REPACK: error.
permutation r_begin o_delete p_repack r_t r_commit

# Open question 7: each read path is refused.
permutation r_begin p_repack r_view r_commit
permutation r_begin p_repack r_srf r_commit
permutation r_begin p_repack r_parallel r_commit
permutation r_begin p_repack_p1 r_parent r_commit

# Accepted spurious errors.
# 16: rows inserted into the empty table after the snapshot, then REPACK.
permutation r_begin o_insert_e p_repack_e r_e r_commit
# A table created after the snapshot, then repacked.
permutation r_begin o_create_n p_repack_n r_n r_commit
