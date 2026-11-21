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
# A session whose record cache was loaded, holding an earlier REPACK's
# record, must also get the error once a later REPACK is replayed (step 7).
#
# Records are never deleted, so the primary's VACUUM finds no record to
# remove. Were a record deleted by the transaction that inserted it, VACUUM
# would remove it and the standby would replay the removal without a recovery
# conflict (heapam.c, HeapTupleHeaderAdvanceConflictHorizon ignores rows
# deleted by their inserting transaction), and this snapshot would read the
# table as empty (doc/research/fix-design.md, step 6).

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
# Autovacuum off: its statistics updates of the record table carry their own
# relcache invalidation, which would reload a warm cache for an unrelated
# reason and hide a missing invalidation from the writer.
$primary->append_conf('postgresql.conf', q{
shared_preload_libraries = 'pg_repack_safe'
autovacuum = off
});
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

# Run a query in a standby session; return (stdout, stderr).
sub runIn
{
    my ($session, $sql) = @_;
    $session->{stderr} = '';
    my $out = $session->query($sql);
    my $err = $session->{stderr};
    $session->{stderr} = '';
    return ($out, $err);
}

my $guardError = qr/cannot read table "t" with this snapshot/;

# Warm cache (step 7). It runs before any VACUUM of the record table: that
# VACUUM's statistics update carries its own invalidation of the record
# table, which would reload the cache for an unrelated reason.
$primary->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');
$primary->wait_for_catchup($standby);

my $warm = $standby->background_psql('postgres', on_error_stop => 0);
is($warm->query_safe('SELECT count(*) FROM t'), '5',
    'standby: warm session reads every row after the first REPACK');
$warm->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$warm->query_safe('SELECT count(*) FROM other');

$primary->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');
$primary->wait_for_catchup($standby);

my ($out, $err) = runIn($warm, 'SELECT count(*) FROM t');
like($err, $guardError,
    'standby: a warm cache still learns of a later REPACK');
isnt($out, '0', 'standby: warm session does not read t as empty');
$warm->query_safe('ROLLBACK');
$warm->quit;

# A standby snapshot taken before a REPACK, without touching t, read after
# the primary has also vacuumed the record table.
my $reader = $standby->background_psql('postgres', on_error_stop => 0);
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$reader->query_safe('SELECT count(*) FROM other');

$primary->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');
$primary->safe_psql('postgres', 'VACUUM pg_repack_safe_rewrites');
$primary->wait_for_catchup($standby);

# The README's check before removing the extension ("Removing the
# extension", step 2) lists this standby session, and only on the standby.
my $oldSnapshots = q{
    SELECT pid FROM pg_stat_activity
    WHERE backend_xmin IS NOT NULL
      AND age(backend_xmin) >= (SELECT age(max(rewrite_xid)::xid)
                                FROM pg_repack_safe_rewrites)};
my $readerPid = $reader->query_safe('SELECT pg_backend_pid()');
is($standby->safe_psql('postgres', $oldSnapshots), $readerPid,
    'README check on the standby lists the old snapshot');
is($primary->safe_psql('postgres', $oldSnapshots), '',
    'README check on the primary does not see it');

($out, $err) = runIn($reader, 'SELECT count(*) FROM t');
like($err, $guardError,
    'standby: old snapshot gets the error after the REPACK and a VACUUM');
isnt($out, '0', 'standby: old snapshot does not read t as empty');
$reader->query_safe('ROLLBACK');
$reader->quit;

is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'), '5',
    'standby: new snapshot reads every row');

$standby->stop;
$primary->stop;
done_testing();
