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
# - o_record: a record written in a transaction that is still open when the
#   reader takes its snapshot, and that commits afterwards. The reader's
#   cache was loaded before the record existed; the invalidation sent with
#   the record arrives at the commit. A REPACK step does not stop between
#   writing its record and committing, so the record is written by
#   test_insert_record (test/modules/test_record), which does what the writer
#   does: simple_heap_insert and a relcache invalidation of the record table,
#   no trigger. The same under session_replication_role = replica: the
#   writer's path does not depend on triggers, so the setting changes
#   nothing.
#
# - o_truncate: the one SQL statement that can change the records, a
#   TRUNCATE under pg_repack_safe.allow_unprotected_drop. The reader, whose
#   cache holds the record and whose snapshot needs it, then reads t as
#   empty: that is what the override gives up. Its cache learns of the
#   TRUNCATE from the invalidation PostgreSQL sends for the table's new
#   file; no trigger of the extension is involved. Were the cache to stay
#   stale instead, the reader would get the error, which is the safe side.

setup
{
    CREATE EXTENSION IF NOT EXISTS pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
    CREATE TABLE other (id int);
    CREATE FUNCTION test_insert_record(record_table regclass, relid oid,
                                       rewrite_xid xid8)
    RETURNS void
    AS 'test_record'
    LANGUAGE C STRICT;
}

teardown
{
    DROP TABLE t, other;
    DROP FUNCTION test_insert_record(regclass, oid, xid8);
}

session reader
step r_warm      { SELECT count(*) FROM t; }
step r_begin     { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step r_t         { SELECT count(*) FROM t; }
step r_commit    { COMMIT; }
step r_begin_sp  { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; SAVEPOINT s; }
step r_back      { ROLLBACK TO s; }

session repacker
step p_repack    { REPACK (CONCURRENTLY) t; }

session other
step o_begin     { BEGIN; }
step o_replica   { SET session_replication_role = replica; BEGIN; }
step o_record    { SELECT test_insert_record('pg_repack_safe_rewrites', 't'::regclass, pg_current_xact_id()); }
step o_commit    { COMMIT; RESET session_replication_role; }
step o_truncate  { SET pg_repack_safe.allow_unprotected_drop = on; TRUNCATE pg_repack_safe_rewrites; RESET pg_repack_safe.allow_unprotected_drop; }

# Cache loaded with no record for t.
permutation r_warm r_begin p_repack r_t r_commit
# Cache loaded with an earlier record for t, which the reader's snapshot sees.
permutation p_repack r_warm r_begin p_repack r_t r_commit
# A record whose transaction is open when the reader takes its snapshot.
permutation r_warm o_begin o_record r_begin o_commit r_t r_commit
# The same under session_replication_role = replica.
permutation r_warm o_replica o_record r_begin o_commit r_t r_commit
# The records are truncated under the override while a reader needs one.
permutation r_begin_sp p_repack r_t r_back o_truncate r_t r_commit
