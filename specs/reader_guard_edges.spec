# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Edge cases of the reader guard (doc/research/fix-design.md, "Interleavings
# that would prove each claim", and research question 7).
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
#   plan, and the parent of a repacked partition (research question 7): each
#   puts the repacked relation in the range table the check reads. Parallel
#   workers are launched on the first ExecGather call (nodeGather.c, ExecGather),
#   after the leader's ExecutorStart has run the check.
# An INSERT's target is written, not read (nodeModifyTable.c, ExecInsert:
# uniqueness and exclusion checks use a dirty snapshot), so it is not
# checked. No error expected:
# - INSERT ... VALUES, then a read of the target: the read still errors;
# - a data-modifying CTE whose INSERT's target the outer query does not read.
# Error expected, each a read of the target:
# - INSERT ... SELECT from the target, and a VALUES subquery on it: a second
#   range-table entry, checked like any read;
# - a CTE's INSERT whose target the outer query reads;
# - ON CONFLICT DO NOTHING, DO UPDATE and DO SELECT on a conflicting key:
#   stock PostgreSQL's own serialization failure, since the conflicting row
#   is invisible to the snapshot (nodeModifyTable.c, ExecCheckTupleVisible);
# - an INSERT into a table with a foreign key to itself: the check reads the
#   target outside the executor (ri_triggers.c, ri_FastPathCheck), so the
#   target stays checked.
# Accepted spurious errors (fix-design.md section 6b), pinned:
# - rows inserted into an empty table after the snapshot, then REPACK (16):
#   stock PostgreSQL returns a correct 0;
# - a table created after the snapshot and then repacked: same cause.
# Error, but stock PostgreSQL's and not the guard's:
# - the foreign-key check of an INSERT into a table that references a
#   repacked table. PostgreSQL 19 checks the referenced table's index with
#   the transaction snapshot outside the executor (ri_triggers.c,
#   ri_FastPathCheck), so the guard does not see the read. The referenced
#   row is invisible to the old snapshot, and the INSERT fails with a
#   foreign-key violation (SQLSTATE 23503) instead of a serialization
#   failure. No wrong rows, but an error a client would not retry
#   (doc/limits.md).
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
    CREATE TABLE s (id int PRIMARY KEY, parent int REFERENCES s);
    INSERT INTO s SELECT generate_series(1, 5);
    CREATE TABLE pk (id int PRIMARY KEY);
    INSERT INTO pk SELECT generate_series(1, 3);
    CREATE TABLE fk (id int REFERENCES pk);
}

teardown
{
    DROP TABLE IF EXISTS n;
    DROP TABLE p, s, fk, pk;
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
step r_ins       { INSERT INTO t VALUES (6, 6); }
step r_ins_sel   { INSERT INTO t SELECT id + 10, v FROM t; }
step r_ins_sub   { INSERT INTO t VALUES (7, (SELECT count(*) FROM t)); }
step r_cte       { WITH i AS (INSERT INTO t VALUES (8, 8) RETURNING id) SELECT count(*) FROM i; }
step r_cte_read  { WITH i AS (INSERT INTO t VALUES (8, 8) RETURNING id) SELECT count(*) FROM t; }
step r_ins_nothing { INSERT INTO t VALUES (1, 1) ON CONFLICT DO NOTHING; }
step r_ins_update { INSERT INTO t VALUES (1, 1) ON CONFLICT (id) DO UPDATE SET v = t.v + 1; }
step r_ins_select { INSERT INTO t VALUES (1, 1) ON CONFLICT (id) DO SELECT RETURNING id; }
step r_ins_self  { INSERT INTO s VALUES (6, 1); }
step r_fk        { INSERT INTO fk VALUES (1); }
step r_commit    { COMMIT; }

session repacker
step p_repack    { REPACK (CONCURRENTLY) t; }
step p_repack_e  { REPACK (CONCURRENTLY) e; }
step p_repack_n  { REPACK (CONCURRENTLY) n; }
step p_repack_p1 { REPACK (CONCURRENTLY) p1; }
step p_repack_s  { REPACK (CONCURRENTLY) s; }
step p_repack_pk { REPACK (CONCURRENTLY) pk; }
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

# Research question 7: each read path is refused.
permutation r_begin p_repack r_view r_commit
permutation r_begin p_repack r_srf r_commit
permutation r_begin p_repack r_parallel r_commit
permutation r_begin p_repack_p1 r_parent r_commit

# An INSERT's target is not read: no error, but a later read errors.
permutation r_begin p_repack r_ins r_t r_commit
permutation r_begin p_repack r_cte r_commit
# Reads of an INSERT's target: error.
permutation r_begin p_repack r_ins_sel r_commit
permutation r_begin p_repack r_ins_sub r_commit
permutation r_begin p_repack r_cte_read r_commit
permutation r_begin p_repack r_ins_nothing r_commit
permutation r_begin p_repack r_ins_update r_commit
permutation r_begin p_repack r_ins_select r_commit
permutation r_begin p_repack_s r_ins_self r_commit

# Accepted spurious errors.
# 16: rows inserted into the empty table after the snapshot, then REPACK.
permutation r_begin o_insert_e p_repack_e r_e r_commit
# A table created after the snapshot, then repacked.
permutation r_begin o_create_n p_repack_n r_n r_commit

# A foreign-key check against a repacked table: stock's 23503, not 40001.
permutation r_begin p_repack_pk r_fk r_commit
