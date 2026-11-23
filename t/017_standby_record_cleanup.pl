# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Deleted records on a hot standby, library loaded on both nodes, without
# hot_standby_feedback, so the primary cannot see standby snapshots.
#
# The primary deletes a record and vacuums it away while standby snapshots
# from before the recorded REPACK (CONCURRENTLY) are still alive. Such a
# snapshot must never read the rewritten table: while the deleted record is
# physically present it gets the reader's error, and replaying the record's
# removal ends it (deleteOlderRecords in src/pg_repack_safe.c gives the
# argument; t/002_standby_record_removal.pl shows the PostgreSQL behaviour
# it rests on, on a plain table).
#
# Three standby REPEATABLE READ transactions take their snapshots before the
# REPACK, without touching t:
# - "early" reads t after the record's deletion is replayed, before its
#   removal: the reader's error;
# - "active" is running a query when the removal is replayed: cancelled;
# - "idle" is idle in its transaction then: terminated.
# None of them ever gets a row count for t.
#
# With log_recovery_conflict_waits on, the standby's log names the record
# table as the relation whose cleanup the replay waits on: the README's way
# to tell these conflicts from those of other vacuums.
#
# Then the removal procedure of the README ("Removing the extension"):
# pruned and vacuumed to zero blocks, the extension is dropped without
# pg_repack_safe.allow_unprotected_removal.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;

my $primary = preloadedNode('primary', conf => "autovacuum = off\n");
$primary->start;
$primary->safe_psql('postgres', q{
    CREATE EXTENSION pageinspect;
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
log_recovery_conflict_waits = on
deadlock_timeout = 50ms
});
$standby->start;

# Wait until the standby has replayed everything the primary has written,
# whether or not the primary has flushed it yet: a VACUUM's last records are
# not flushed when the statement returns.
sub replayAll
{
    $primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
    return;
}

# Row versions physically in the record table, deleted or not.
my $versions = q{
    SELECT CASE WHEN pg_relation_size('repack_safe.rewrites') = 0 THEN 0
           ELSE (SELECT count(*) FROM heap_page_items(
                     get_raw_page('repack_safe.rewrites', 0))
                 WHERE lp_len > 0) END};

replayAll();

# The three old snapshots.
my $early = oldSnapshot($standby);
my $idle = oldSnapshot($standby);
my $idlePid = $idle->query_safe('SELECT pg_backend_pid()');
my ($activeOut, $activeErr) = ('', '');
my $activeFile = PostgreSQL::Test::Utils::tempdir() . '/active.sql';
PostgreSQL::Test::Utils::append_to_file($activeFile, q{
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT 'before', count(*) FROM other;
SELECT pg_sleep(600);
SELECT 'after', count(*) FROM t;
});
my $active = IPC::Run::start(
    [ 'psql', '-XAtq', '-d', $standby->connstr('postgres'), '-f', $activeFile ],
    '>', \$activeOut, '2>', \$activeErr);
$standby->poll_query_until('postgres', q{
    SELECT EXISTS (SELECT 1 FROM pg_stat_activity
                   WHERE query LIKE 'SELECT pg_sleep%' AND state = 'active')
}) or die 'the active standby reader never reached pg_sleep';

# The REPACK is recorded, and the record deleted by a later transaction.
$primary->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');
is($primary->safe_psql('postgres',
        'SELECT repack_safe.prune_records()'),
    '1', 'primary: the record is deleted');
replayAll();
is($standby->safe_psql('postgres',
        'SELECT count(*) FROM repack_safe.rewrites'),
    '0', 'standby: the deleted record is invisible to a new snapshot');
is($standby->safe_psql('postgres', $versions),
    '1', 'standby: the deleted record is still physically present');

my ($out, $err) = runIn($early, 'SELECT count(*) FROM t');
like($err, qr/cannot read table "t" with this snapshot/,
    'standby: an old snapshot gets the error from a deleted record');
is($out, '', 'standby: that snapshot gets no row count for t');
$early->query_safe('ROLLBACK');
$early->quit;

# The primary has no snapshot that needs the record, so VACUUM removes it.
my $logOffset = -s $standby->logfile;
my ($dbOid, $filenode) = split /\|/, $primary->safe_psql('postgres', q{
    SELECT d.oid, pg_relation_filenode('repack_safe.rewrites')
    FROM pg_database d WHERE d.datname = current_database()});
$primary->safe_psql('postgres', 'VACUUM repack_safe.rewrites');
is($primary->safe_psql('postgres', $versions),
    '0', 'primary: VACUUM removed the deleted record');

# Replaying the removal ends both remaining old snapshots first.
replayAll();
$active->finish;
like($activeOut, qr/^before\|0$/m, 'standby: the active reader took its snapshot');
like($activeErr, qr/conflict with recovery/,
    'standby: the active reader is cancelled when the record is removed');
unlike($activeOut, qr/^after/m,
    'standby: the active reader gets no row count for t');
is($standby->safe_psql('postgres',
        "SELECT count(*) FROM pg_stat_activity WHERE pid = $idlePid"),
    '0', 'standby: the idle reader is gone when the record is removed');
like(slurp_file($standby->logfile, $logOffset),
    qr/FATAL:  terminating connection due to conflict with recovery/,
    'standby: the idle reader was terminated by the conflict');
eval { $idle->quit; };

# The wait is logged after deadlock_timeout, before the readers are ended,
# with the WAL record being replayed, which names the record table as
# tablespace/database/filenode; the README maps it back with
# pg_filenode_relation.
my $conflictLog = slurp_file($standby->logfile, $logOffset);
like($conflictLog,
    qr/LOG:  recovery still waiting after [\d.]+ ms: recovery conflict on snapshot\n.*DETAIL:  Conflicting processes: .*$idlePid/,
    'standby: the wait on the conflict is logged with the readers it ends');
my ($spcOid) = $conflictLog =~
    m{recovery still waiting.*\n.*\n.*CONTEXT:  WAL redo at .* rel (\d+)/$dbOid/$filenode\b};
ok(defined $spcOid,
    'standby: the logged WAL record names the record table');
is($standby->safe_psql('postgres',
        "SELECT pg_filenode_relation(" . ($spcOid // 'NULL') . ", $filenode)"),
    'repack_safe.rewrites',
    'standby: pg_filenode_relation maps the logged relation to the record table');

is($standby->safe_psql('postgres', $versions),
    '0', 'standby: the record is removed');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
    '5', 'standby: a new snapshot reads every row');

# Removing the extension: nothing is left to prune, the record table has no
# blocks, and DROP EXTENSION needs no override.
is($primary->safe_psql('postgres',
        q{SELECT repack_safe.prune_records(),
                 pg_relation_size('repack_safe.rewrites')}),
    '0|0', 'primary: pruned and vacuumed, the record table has no blocks');
$primary->safe_psql('postgres', 'DROP EXTENSION pg_repack_safe');
replayAll();
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
    '5', 'standby: every row is read after the extension is dropped');

$standby->stop;
$primary->stop;
done_testing();
