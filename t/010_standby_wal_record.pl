# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The custom WAL record written at the swap of REPACK (CONCURRENTLY)
# (doc/research/fix-design.md, step 10, decision 6).
#
# A standby without the library cannot check reads, so it must stop replay
# before it applies a REPACK (CONCURRENTLY): the record precedes the
# REPACK's commit, and replaying a record of an unregistered resource manager
# raises an error in the startup process (rmgr.c, RmgrNotFound). A standby
# with the library replays the record (a no-op) and keeps protecting its old
# snapshots. The primary's own crash recovery replays it too.
#
# Known gap, not tested here (open question 17): a standby without the
# library whose replay starts from a checkpoint between the swap and the
# commit never sees the record. Placing a checkpoint there needs injection
# points, which this build lacks.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;
use Time::HiRes qw(sleep);

my $primary = preloadedNode('primary');
$primary->start;
$primary->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
    CREATE TABLE other (id int);
});
$primary->backup('base');

my $bare = PostgreSQL::Test::Cluster->new('bare');
$bare->init_from_backup($primary, 'base', has_streaming => 1);
$bare->append_conf('postgresql.conf', "shared_preload_libraries = ''\n");
$bare->start;

my $guarded = PostgreSQL::Test::Cluster->new('guarded');
$guarded->init_from_backup($primary, 'base', has_streaming => 1);
$guarded->start;

$primary->wait_for_catchup($bare);
$primary->wait_for_catchup($guarded);
is($bare->safe_psql('postgres', 'SELECT count(*) FROM t'), '5',
    'standby without the library serves reads before any REPACK');

my $reader = oldSnapshot($guarded);

my $beforeRepack = $primary->safe_psql('postgres',
    'SELECT pg_current_wal_insert_lsn()');
$primary->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');
my $afterRepack = $primary->safe_psql('postgres',
    'SELECT pg_current_wal_insert_lsn()');

# The standby with the library replays past the REPACK and refuses the old
# snapshot's read.
$primary->wait_for_catchup($guarded);
my ($out, $err) = runIn($reader, 'SELECT count(*) FROM t');
like($err, qr/cannot read table "t" with this snapshot/,
    'standby with the library: old snapshot gets the error');
$reader->quit;

# The standby without the library stops at the record.
ok( $bare->wait_for_log(
        qr/resource manager with ID 149 not registered/),
    'standby without the library: replay fails on the record');
my $stopped = 0;
foreach (1 .. 100)
{
    if (system('pg_ctl', 'status', '--pgdata' => $bare->data_dir) != 0)
    {
        $stopped = 1;
        last;
    }
    sleep 0.1;
}
ok($stopped, 'standby without the library is down, serving no reads');
$bare->_update_pid(0);

# In the primary's WAL the record belongs to the REPACK's transaction and
# precedes that transaction's commit, so replay stopped before the commit.
my ($waldump) = run_command([
    'pg_waldump', '--path' => $primary->data_dir . '/pg_wal',
    '--start' => $beforeRepack, '--end' => $afterRepack ]);
my ($recordXid, $recordLsn) =
  $waldump =~ /rmgr: custom149 .*?tx:\s+(\d+), lsn: (\S+),/;
ok(defined $recordXid, 'the REPACK wrote the pg_repack_safe record');
my ($commitLsn) =
  $waldump =~ /rmgr: Transaction .*?tx:\s+$recordXid, lsn: (\S+), .*desc: COMMIT/;
ok(defined $commitLsn, "the record's transaction commits");
is( $primary->safe_psql('postgres',
        "SELECT '$recordLsn'::pg_lsn < '$commitLsn'::pg_lsn"),
    't', 'the record precedes the commit');

# Restarted with the library, the standby replays the record and catches up
# (doc/installation.md).
$bare->append_conf('postgresql.conf',
    "shared_preload_libraries = 'pg_repack_safe'\n");
$bare->start;
$primary->wait_for_catchup($bare);
is($bare->safe_psql('postgres', 'SELECT count(*) FROM t'), '5',
    'standby restarted with the library resumes replay');
$bare->stop;

# The primary's crash recovery replays the record.
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', 'SELECT count(*) FROM t'), '5',
    'primary recovers through the record');

$guarded->stop;
$primary->stop;
done_testing();
