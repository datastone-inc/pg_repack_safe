# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The reader guard on a hot standby, library loaded on both nodes, without
# hot_standby_feedback, so the primary cannot see standby snapshots.
#
# A standby REPEATABLE READ snapshot taken before REPACK (CONCURRENTLY) on
# the primary must get the error once the REPACK is replayed. Once the
# primary has vacuumed the record table, which sees no standby snapshot,
# replaying that VACUUM ends any such snapshot still alive: it never reads
# the table (t/017_standby_record_cleanup.pl covers deleted records in
# full).
#
# A session whose record cache was loaded, holding an earlier REPACK's
# record, must also get the error once a later REPACK is replayed (step 7).

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;

# Autovacuum off: its statistics updates of the record table carry their own
# relcache invalidation, which would reload a warm cache for an unrelated
# reason and hide a missing invalidation from the writer.
my $primary = preloadedNode('primary', conf => "autovacuum = off\n");
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
# the REPACK is replayed: the reader's error.
my $reader = oldSnapshot($standby);
my $idle = oldSnapshot($standby);
my $idlePid = $idle->query_safe('SELECT pg_backend_pid()');

$primary->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');
$primary->wait_for_catchup($standby);

($out, $err) = runIn($reader, 'SELECT count(*) FROM t');
like($err, $guardError,
    'standby: old snapshot gets the error after the REPACK');
isnt($out, '0', 'standby: old snapshot does not read t as empty');
$reader->query_safe('ROLLBACK');
$reader->quit;

# The primary then vacuums the record table. That REPACK deleted the
# earlier records, which VACUUM removes, and the page is marked all-visible;
# either gives the VACUUM a conflict horizon no older than the REPACK
# (pruneheap.c, heap_page_prune_and_freeze), so replaying it ends the other
# old snapshot, which is idle in its transaction. It never reads t. The
# wait is for the primary's insert position: a VACUUM's last records are not
# flushed when the statement returns, and the default target would be
# reached before they are replayed.
my $logOffset = -s $standby->logfile;
$primary->safe_psql('postgres', 'VACUUM repack_safe.rewrites');
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
is($standby->safe_psql('postgres',
        "SELECT count(*) FROM pg_stat_activity WHERE pid = $idlePid"),
    '0', 'standby: the other old snapshot is ended when the VACUUM is replayed');
like(slurp_file($standby->logfile, $logOffset),
    qr/FATAL:  terminating connection due to conflict with recovery/,
    'standby: it was terminated by the recovery conflict');
eval { $idle->quit; };

is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'), '5',
    'standby: new snapshot reads every row');

$standby->stop;
$primary->stop;
done_testing();
