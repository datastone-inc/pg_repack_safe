# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Stock behaviour, pinned with the extension loaded and created: commands
# other than REPACK (CONCURRENTLY) that replace a table's storage without
# preserving row visibility. The extension does not guard them (design
# decision 4; README "What pg_repack_safe does not guarantee"). A REPEATABLE
# READ snapshot taken before the command, without touching the table, then
# reads wrong rows, as on stock PostgreSQL 19. The expected output pins those
# wrong rows, so that a change in either PostgreSQL or the extension shows
# up here; if the guard is ever extended to one of these commands, its
# permutation flips to the guard's error.
#
# - TRUNCATE: the old snapshot reads the table as empty (documented in
#   ref/truncate.sgml as not MVCC-safe).
# - A rewriting ALTER TABLE (a column type change that needs conversion):
#   the old snapshot reads the table as empty (ref/alter_table.sgml).
# - REFRESH MATERIALIZED VIEW without CONCURRENTLY (research questions 4, 16):
#   the old snapshot reads the view's new contents, 4 rows instead of the
#   3 it had when the snapshot was taken. The refresh writes its rows frozen
#   (matview.c, transientrel_startup, TABLE_INSERT_FROZEN), so every snapshot sees them; the
#   reference page does not say the command is not MVCC-safe.
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
    CREATE TABLE other (id int);
}

teardown
{
    DROP MATERIALIZED VIEW mv;
    DROP TABLE t, a, other;
}

session reader
step r_begin     { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step r_t         { SELECT count(*), sum(v) FROM t; }
step r_a         { SELECT count(*), sum(v) FROM a; }
step r_mv        { SELECT count(*), sum(v) FROM mv; }
step r_commit    { COMMIT; }

session writer
step w_truncate  { TRUNCATE t CASCADE; }
step w_alter     { ALTER TABLE a ALTER COLUMN v TYPE bigint; }
step w_refresh   { REFRESH MATERIALIZED VIEW mv; }
step w_refresh_c { REFRESH MATERIALIZED VIEW CONCURRENTLY mv; }

# Stock: 0 rows instead of 4.
permutation r_begin w_truncate r_t r_commit
# Stock: 0 rows instead of 4.
permutation r_begin w_alter r_a r_commit
# Stock: 4 rows (the new contents) instead of 3.
permutation r_begin w_refresh r_mv r_commit
# Control: 3 rows.
permutation r_begin w_refresh_c r_mv r_commit
