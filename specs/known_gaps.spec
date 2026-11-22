# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# KNOWN GAPS, pinned (doc/research/fix-design.md, step 12): commands that
# replace a table's storage without preserving row visibility, which the
# extension does not record (design decision 4). A REPEATABLE READ snapshot
# taken before the command, without touching the table, then reads wrong
# rows, with the extension loaded and created. The expected output pins
# those wrong rows; when one of these gaps is closed, its permutation flips
# to the reader guard's error.
#
# - TRUNCATE: the old snapshot reads the table as empty (documented in
#   ref/truncate.sgml as not MVCC-safe).
# - A rewriting ALTER TABLE (a column type change that needs conversion):
#   the old snapshot reads the table as empty (ref/alter_table.sgml).
# - REFRESH MATERIALIZED VIEW without CONCURRENTLY (open questions 4, 16):
#   the old snapshot reads the view's new contents, 4 rows instead of the
#   3 it had when the snapshot was taken. The refresh writes its rows frozen
#   (matview.c, transientrel_startup, TABLE_INSERT_FROZEN), so every snapshot sees them; the
#   reference page does not say the command is not MVCC-safe.
# - The foreign-key check of an INSERT into a table that references a
#   repacked table: PostgreSQL 19 checks the referenced table's index with
#   the transaction snapshot outside the executor (ri_triggers.c, ri_FastPathCheck),
#   so the reader guard does not see the read. The referenced row is
#   invisible to the old snapshot, and the INSERT fails with a foreign-key
#   violation (SQLSTATE 23503) instead of a serialization failure. No wrong
#   rows, but an error a client would not retry.
# Control: REFRESH MATERIALIZED VIEW CONCURRENTLY updates the view in place
# with ordinary row changes, so the old snapshot still reads its 3 rows.

setup
{
    CREATE EXTENSION IF NOT EXISTS pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY, v int NOT NULL);
    INSERT INTO t SELECT g, g FROM generate_series(1, 3) g;
    CREATE MATERIALIZED VIEW mv AS SELECT id, v FROM t;
    CREATE UNIQUE INDEX mv_id ON mv (id);
    INSERT INTO t VALUES (4, 4);
    CREATE TABLE a (id int PRIMARY KEY, v int NOT NULL);
    INSERT INTO a SELECT g, g FROM generate_series(1, 4) g;
    CREATE TABLE pk (id int PRIMARY KEY);
    INSERT INTO pk SELECT generate_series(1, 3);
    CREATE TABLE fk (id int REFERENCES pk);
    CREATE TABLE other (id int);
}

teardown
{
    DROP MATERIALIZED VIEW mv;
    DROP TABLE t, a, fk, pk, other;
}

session reader
step r_begin     { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step r_t         { SELECT count(*), sum(v) FROM t; }
step r_a         { SELECT count(*), sum(v) FROM a; }
step r_mv        { SELECT count(*), sum(v) FROM mv; }
step r_fk        { INSERT INTO fk VALUES (1); }
step r_commit    { COMMIT; }

session writer
step w_truncate  { TRUNCATE t CASCADE; }
step w_alter     { ALTER TABLE a ALTER COLUMN v TYPE bigint; }
step w_refresh   { REFRESH MATERIALIZED VIEW mv; }
step w_refresh_c { REFRESH MATERIALIZED VIEW CONCURRENTLY mv; }
step w_repack_pk { REPACK (CONCURRENTLY) pk; }

# KNOWN GAP: 0 rows instead of 4.
permutation r_begin w_truncate r_t r_commit
# KNOWN GAP: 0 rows instead of 4.
permutation r_begin w_alter r_a r_commit
# KNOWN GAP: 4 rows (the new contents) instead of 3.
permutation r_begin w_refresh r_mv r_commit
# Control: 3 rows.
permutation r_begin w_refresh_c r_mv r_commit
# KNOWN GAP: a foreign-key violation instead of the guard's error.
permutation r_begin w_repack_pk r_fk r_commit
