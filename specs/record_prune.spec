# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Deleting a record must not let a snapshot that needs it read the
# rewritten table.
#
# The reader takes a REPEATABLE READ snapshot without touching t, and t is
# then repacked. The record is deleted (repack_safe.prune_records) and
# the record table vacuumed. The reader's read of t must still fail: VACUUM
# cannot remove the deleted row version while the reader's snapshot is
# alive, and the reader loads every row version.
# - r_warm: the same with the reader's cache loaded before the deletion.
# - Two sessions pruning at once: the second does not wait for the first and
#   deletes nothing the first already has; neither fails.
# - A REPACK (CONCURRENTLY) deletes older records itself: the reader between
#   two repacks still fails. A repack started while another transaction is
#   deleting records waits for that transaction, as PostgreSQL makes it wait
#   for any transaction that has written, and then succeeds.
# - After the reader's transaction ends, VACUUM returns the record table to
#   zero blocks and a new snapshot reads every row.

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
step r_t         { SELECT count(*) FROM t; }
step r_commit    { COMMIT; }

session writer
step w_repack    { REPACK (CONCURRENTLY) t; }
step w_prune     { SELECT repack_safe.prune_records() > 0 AS deleted_some; }
step w_vacuum    { VACUUM repack_safe.rewrites; }
step w_size      { SELECT pg_relation_size('repack_safe.rewrites') > 0 AS has_blocks; }
step w_begin     { BEGIN; }
step w_commit    { COMMIT; }

session other
step o_prune     { SELECT repack_safe.prune_records() > 0 AS deleted_some; }
step o_repack    { REPACK (CONCURRENTLY) t; }
step o_t         { SELECT count(*) FROM t; }

# The record is deleted and vacuumed while a snapshot needs it.
permutation r_begin w_repack w_prune w_vacuum w_size r_t r_commit w_vacuum w_size o_t
# The reader loads the record only after it was deleted and vacuumed.
permutation r_begin w_repack w_prune w_vacuum r_t r_t r_commit
# Two records, the reader between them.
permutation w_repack r_begin w_repack w_prune w_vacuum r_t r_commit
# A snapshot taken after the repack reads the table, record deleted or not.
permutation w_repack r_begin w_prune w_vacuum r_t r_commit
# The second repack deletes the first one's record; no prune.
permutation w_repack r_begin w_repack w_vacuum r_t r_commit
permutation r_begin w_repack w_repack w_vacuum r_t r_commit
# A repack is started while another transaction is deleting records.
permutation w_repack w_begin w_prune o_repack w_commit o_t
# Two sessions pruning at once.
permutation w_repack w_begin w_prune o_prune w_commit o_prune
