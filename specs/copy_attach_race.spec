# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# COPY of a partitioned table must not scan a partition it has not checked.
#
# The COPY guard lists the partitions and checks each; DoCopy then lists them
# again (copyto.c, BeginCopyTo) and scans that second list. ATTACH PARTITION
# takes ShareUpdateExclusiveLock on the parent (tablecmds.c,
# AlterTableGetLockLevel), which the reader's AccessShareLock does not block.
# A table repacked after the reader's snapshot and attached between the two
# listings would be scanned unchecked and read as empty.
#
# - attach_during: the reader's COPY waits for a lock on p1 while it lists
#   the partitions. The ATTACH of the repacked p2 must wait for the COPY to
#   end, so the COPY reads p as it was: p1's rows, and p2 is not scanned.
# - attach_before: the ATTACH commits before the COPY starts, so the COPY
#   finds p2 among the partitions and is refused.
#
# isolationtester cannot receive COPY ... TO STDOUT, so the COPY writes a
# server file, which the same function reads back.

setup
{
    CREATE EXTENSION IF NOT EXISTS pg_repack_safe;
    CREATE TABLE p (id int PRIMARY KEY) PARTITION BY RANGE (id);
    CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (1) TO (100);
    INSERT INTO p SELECT generate_series(1, 3);
    CREATE TABLE p2 (id int PRIMARY KEY);
    INSERT INTO p2 SELECT generate_series(101, 103);
    CREATE TABLE other (id int);
    CREATE FUNCTION copy_p() RETURNS text
    LANGUAGE plpgsql
    AS $$
    DECLARE
        target text := current_setting('data_directory') || '/copy_attach_race.out';
    BEGIN
        EXECUTE format('COPY p TO %L', target);
        RETURN pg_read_file(target);
    END
    $$;
}

teardown
{
    DROP TABLE p, other;
    DROP TABLE IF EXISTS p2;
    DROP FUNCTION copy_p();
}

session reader
step r_begin     { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step r_copy      { SELECT copy_p(); }
step r_commit    { COMMIT; }

session repacker
step p_repack_p2 { REPACK (CONCURRENTLY) p2; }
step p_attach    { ALTER TABLE p ATTACH PARTITION p2 FOR VALUES FROM (100) TO (200); }
step p_count     { SELECT count(*) FROM p; }

session other
step o_lock      { BEGIN; LOCK TABLE p1 IN ACCESS EXCLUSIVE MODE; }
step o_commit    { COMMIT; }

# attach_during
permutation r_begin p_repack_p2 o_lock r_copy p_attach o_commit r_commit(p_attach) p_count
# attach_before
permutation r_begin p_repack_p2 p_attach r_copy r_commit p_count
