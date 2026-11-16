# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Logical replication's initial table synchronization racing
# REPACK (CONCURRENTLY) on the publisher (doc/research/fix-design.md, open
# question 2).
#
# The table sync worker runs, on a replication connection to the publisher,
# BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ, then creates its slot with
# SNAPSHOT 'use' (tablesync.c:1414-1431), then copies the table with
# COPY ... TO (tablesync.c:1113). If REPACK (CONCURRENTLY) commits after the
# slot's snapshot and before the COPY takes its lock, stock PostgreSQL copies
# no rows and the subscriber's table stays without its initial rows.
#
# Driving a real subscription does not reach that order deterministically:
# CREATE SUBSCRIPTION reads the table on the publisher (to list the
# publication's tables) and waits behind the REPACK, and a slot created
# while the REPACK holds an XID waits for it to end before its snapshot is
# consistent. So this test runs the sync worker's commands itself on a
# replication connection: the snapshot is taken before the REPACK starts,
# and the COPY queues behind the REPACK's ACCESS EXCLUSIVE request (a
# blocker holds ACCESS SHARE, as in t/001). The COPY check refuses the COPY,
# which a sync worker reports and retries.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
$publisher->append_conf('postgresql.conf',
    "shared_preload_libraries = 'pg_repack_safe'\n");
$publisher->start;
$publisher->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
});

# The sync worker's first two steps.
my $sync = $publisher->background_psql('postgres',
    on_error_stop => 0,
    connstr => $publisher->connstr('postgres') . ' replication=database');
$sync->query_safe('BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ');
$sync->query_safe(
    q{CREATE_REPLICATION_SLOT sync_slot TEMPORARY LOGICAL pgoutput (SNAPSHOT 'use')});

my $blocker = $publisher->background_psql('postgres');
$blocker->query_safe('BEGIN');
$blocker->query_safe('SELECT count(*) FROM t');

my $repacker = $publisher->background_psql('postgres');
$repacker->query_until(qr/repack_started/,
    "\\echo repack_started\nREPACK (CONCURRENTLY) t;\n");
$publisher->poll_query_until('postgres', q{
    SELECT EXISTS (SELECT 1 FROM pg_locks
                   WHERE relation = 't'::regclass
                     AND mode = 'AccessExclusiveLock' AND NOT granted)
}) or die 'REPACK never queued for ACCESS EXCLUSIVE on t';

# The sync worker's COPY queues behind the REPACK.
$sync->query_until(qr/copy_started/, "\\echo copy_started\nCOPY t TO STDOUT;\n");
$publisher->poll_query_until('postgres', q{
    SELECT EXISTS (SELECT 1 FROM pg_locks l
                   JOIN pg_stat_activity a USING (pid)
                   WHERE l.relation = 't'::regclass
                     AND l.mode = 'AccessShareLock' AND NOT l.granted
                     AND a.backend_type = 'walsender')
}) or die 'the COPY never queued behind REPACK';

$blocker->query_safe('COMMIT');
$blocker->quit;

# Collect the COPY's outcome. Stock PostgreSQL 19: no error and no rows.
$sync->{stderr} = '';
my $out = $sync->query('\\echo copy_done');
$out =~ s/^copy_done$//m;
like($sync->{stderr}, qr/cannot read table "t" with this snapshot/,
    'the sync COPY with a pre-REPACK snapshot is refused');
my $rows = () = $out =~ /^\d+$/mg;
is($rows, 0, 'the sync COPY wrote no rows');

$sync->quit;
$repacker->quit;
$publisher->stop;
done_testing();
