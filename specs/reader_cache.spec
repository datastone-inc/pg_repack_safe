# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The reader's per-backend record cache must not hide a record written after
# it was loaded (doc/research/fix-design.md, step 7).
#
# In each permutation the reader first runs a query in its own transaction,
# which loads its cache, then takes a REPEATABLE READ snapshot without
# touching t. A record for t then commits; the reader's read of t must fail.
# - p_repack: the writer's own invalidation, with the cache loaded empty and
#   with the cache already holding an earlier record for t;
# - o_record: a record inserted by hand reaches the cache through the record
#   table's statement trigger, also under session_replication_role = replica,
#   which skips triggers not enabled ALWAYS.

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
step r_warm      { SELECT count(*) FROM t; }
step r_begin     { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step r_t         { SELECT count(*) FROM t; }
step r_commit    { COMMIT; }

session repacker
step p_repack    { REPACK (CONCURRENTLY) t; }

session other
step o_begin     { BEGIN; }
step o_replica   { SET session_replication_role = replica; BEGIN; }
step o_record    { INSERT INTO pg_repack_safe_rewrites VALUES ('t'::regclass, pg_current_xact_id()); }
step o_commit    { COMMIT; RESET session_replication_role; }

# Cache loaded with no record for t.
permutation r_warm r_begin p_repack r_t r_commit
# Cache loaded with an earlier record for t, which the reader's snapshot sees.
permutation p_repack r_warm r_begin p_repack r_t r_commit
# A record inserted by hand.
permutation r_warm o_begin o_record r_begin o_commit r_t r_commit
# A record inserted by hand under session_replication_role = replica.
permutation r_warm o_replica o_record r_begin o_commit r_t r_commit
