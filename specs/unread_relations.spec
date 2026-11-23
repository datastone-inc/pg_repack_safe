# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The reader check passes a query that names a repacked table without
# reading it.
#
# A plan's range table also holds relations the plan does not scan, and the
# executor never opens them (execUtils.c, ExecGetRangeTableRelation), so the
# reader check passes them:
# - a table the planner removed as a useless left join (analyzejoins.c,
#   remove_useless_joins leaves the range-table entry in place);
# - a table whose scan the planner replaced with an empty Result because its
#   quals are constant false;
# - a partition that run-time pruning removes when the executor starts
#   (execPartition.c, ExecDoInitialPruning).
# Each query returns what stock PostgreSQL returns, which is correct: no row
# of the repacked table is read.
#
# The controls read the repacked table through the same statements and are
# refused: the same join with a column of t in the select list, which keeps
# the join; a qual that is not constant; and the same prepared statement
# with a parameter in the repacked partition.

setup
{
    CREATE EXTENSION IF NOT EXISTS pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY, v int NOT NULL);
    INSERT INTO t SELECT g, g FROM generate_series(1, 5) g;
    CREATE TABLE other (id int PRIMARY KEY);
    INSERT INTO other VALUES (1), (2);
    CREATE TABLE p (id int PRIMARY KEY) PARTITION BY RANGE (id);
    CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (1) TO (100);
    CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (100) TO (200);
    INSERT INTO p VALUES (1), (2), (150);
}

teardown
{
    DROP TABLE t, other, p;
}

session reader
step r_prepare    { PREPARE in_p(int) AS SELECT count(*) FROM p WHERE id = $1; SET plan_cache_mode = force_generic_plan; }
step r_begin      { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step r_join       { SELECT count(*) FROM other o LEFT JOIN t ON o.id = t.id; }
step r_join_read  { SELECT count(t.v) FROM other o LEFT JOIN t ON o.id = t.id; }
step r_false      { SELECT count(*) FROM t WHERE false; }
step r_qual_read  { SELECT count(*) FROM t WHERE v < 0; }
step r_pruned     { EXECUTE in_p(150); }
step r_unpruned   { EXECUTE in_p(1); }
step r_rollback   { ROLLBACK; }
teardown          { DEALLOCATE ALL; RESET plan_cache_mode; }

session repacker
step p_repack    { REPACK (CONCURRENTLY) t; }
step p_repack_p1 { REPACK (CONCURRENTLY) p1; }

# The join to t is removed: the plan scans only "other". 2 rows.
permutation r_begin p_repack r_join r_rollback
# Control: the join is kept, t is read.
permutation r_begin p_repack r_join_read r_rollback
# The scan of t is replaced by an empty Result. 0 rows.
permutation r_begin p_repack r_false r_rollback
# Control: t is scanned.
permutation r_begin p_repack r_qual_read r_rollback
# p1 is pruned at executor start; only p2 is scanned. 1 row.
permutation r_prepare r_begin p_repack_p1 r_pruned r_rollback
# Control: p1 is not pruned.
permutation r_prepare r_begin p_repack_p1 r_unpruned r_rollback
