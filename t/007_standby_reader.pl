# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The reader guard on a hot standby, library loaded on both nodes, without
# hot_standby_feedback, so the primary cannot see standby snapshots.
#
# A standby REPEATABLE READ snapshot taken before REPACK (CONCURRENTLY) on
# the primary must get the error once the REPACK is replayed, and must still
# get it after the primary has vacuumed the record table: VACUUM there sees
# no snapshot that needs the record.
#
# This pins the hazard found while building record cleanup
# (doc/research/fix-design.md, step 6): when a record was deleted by the
# transaction that inserted it, the primary's VACUUM removed it and the
# standby replayed the removal without a recovery conflict
# (heapam.c, HeapTupleHeaderAdvanceConflictHorizon ignores rows deleted by
# their inserting transaction), so this snapshot read the table as empty.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
    "shared_preload_libraries = 'pg_repack_safe'\n");
$primary->start;
$primary->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
    CREATE TABLE other (id int);
});

$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', q{
hot_standby_feedback = off
max_standby_streaming_delay = 200ms
});
$standby->start;
$primary->wait_for_catchup($standby);

# A standby snapshot taken before the REPACK, without touching t.
my $reader = $standby->background_psql('postgres', on_error_stop => 0);
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$reader->query_safe('SELECT count(*) FROM other');

$primary->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');
$primary->safe_psql('postgres', 'VACUUM pg_repack_safe_rewrites');
$primary->wait_for_catchup($standby);

$reader->{stderr} = '';
my $out = $reader->query('SELECT count(*) FROM t');
like($reader->{stderr}, qr/cannot read table "t" with this snapshot/,
    'standby: old snapshot gets the error after the REPACK and a VACUUM');
isnt($out, '0', 'standby: old snapshot does not read t as empty');
$reader->{stderr} = '';
$reader->query_safe('ROLLBACK');
$reader->quit;

is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'), '5',
    'standby: new snapshot reads every row');

$standby->stop;
$primary->stop;
done_testing();
