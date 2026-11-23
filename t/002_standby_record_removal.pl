# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Design probe of PostgreSQL's behaviour, for the rule that would let records
# be deleted (doc/research/fix-design.md, section 6c, "Record lifetime", and
# step 6). The extension does not delete records, and refuses DELETE on its
# record table; this test shows what a deletion would rely on, on a hot
# standby without hot_standby_feedback.
#
# The primary cannot see standby snapshots, so it could DELETE and VACUUM a
# record while a standby transaction still needs it. The reader check scans
# the record table with SnapshotAny, so deleting is safe only if both of
# these are true:
#
#   1. After the DELETE is replayed, the row is still physically present on
#      the standby, although no MVCC snapshot taken now sees it.
#   2. The row cannot be physically removed on the standby while a snapshot
#      that predates the row's inserter is alive: replaying the VACUUM
#      cancels that snapshot's query instead.
#
# The last two cases show why the deleter must not be the inserter: a row
# deleted by its own inserting transaction is removed on the standby without
# ending an older snapshot, and a row deleted by a later transaction is not.
#
# This test uses a plain two-column table in place of the record table (its
# transaction ID column is xid, where the record table's is xid8); it does
# not load the extension.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;

$primary->safe_psql('postgres', q{
    CREATE EXTENSION pageinspect;
    CREATE TABLE records (relid oid NOT NULL, rewrite_xid xid NOT NULL);
    CREATE TABLE other (id int);
    INSERT INTO other VALUES (1);
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

# A standby REPEATABLE READ transaction takes its snapshot, then stays busy.
my ($readerOut, $readerErr) = ('', '');
my $readerSql = q{
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT 'before', count(*) FROM other;
SELECT pg_sleep(600);
SELECT 'after', count(*) FROM other;
};
my $readerFile = PostgreSQL::Test::Utils::tempdir() . '/reader.sql';
PostgreSQL::Test::Utils::append_to_file($readerFile, $readerSql);
my $reader = IPC::Run::start(
    [ 'psql', '-XAtq', '-d', $standby->connstr('postgres'), '-f', $readerFile ],
    '>', \$readerOut, '2>', \$readerErr);

$standby->poll_query_until('postgres', q{
    SELECT EXISTS (SELECT 1 FROM pg_stat_activity
                   WHERE query LIKE 'SELECT pg_sleep%' AND state = 'active')
}) or die 'standby reader never reached pg_sleep';

# The primary writes a record after the reader's snapshot, then deletes it.
$primary->safe_psql('postgres', q{
    INSERT INTO records VALUES ('other'::regclass, pg_current_xact_id()::xid);
});
$primary->safe_psql('postgres', 'DELETE FROM records');
$primary->wait_for_catchup($standby);

is($standby->safe_psql('postgres', 'SELECT count(*) FROM records'), 0,
    'standby: deleted record is invisible to a new MVCC snapshot');
is( $standby->safe_psql('postgres', q{
        SELECT count(*) FROM heap_page_items(get_raw_page('records', 0))
        WHERE lp_len > 0
    }),
    1,
    'standby: deleted record is still physically present');
is( $standby->safe_psql('postgres', q{
        SELECT count(*) FROM pg_stat_activity
        WHERE query LIKE 'SELECT pg_sleep%' AND state = 'active'
    }),
    1,
    'standby: reader is unaffected by the DELETE');

# VACUUM removes the row on the primary, where no snapshot needs it.
$primary->safe_psql('postgres', 'VACUUM records');
is($primary->safe_psql('postgres', q{SELECT pg_relation_size('records')}),
    0, 'primary: VACUUM returned the record table to zero blocks');

# Replaying the removal conflicts with the reader's snapshot.
$reader->finish;
like($readerOut, qr/^before\|1$/m, 'standby reader took its snapshot');
like(
    $readerErr,
    qr/conflict with recovery/,
    'standby reader is cancelled when the record is physically removed');
unlike($readerOut, qr/^after/m,
    'standby reader does not continue past the removal');

$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', q{SELECT pg_relation_size('records')}),
    0, 'standby: record table is back to zero blocks');

# The two cases that decide who may delete a record. One idle standby
# transaction holds a snapshot through both.
my $idle = $standby->background_psql('postgres', on_error_stop => 0);
$idle->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$idle->query_safe('SELECT count(*) FROM other');
my $idlePid = $idle->query_safe('SELECT pg_backend_pid()');
my $logOffset = -s $standby->logfile;

# A row deleted by the transaction that inserted it. Its removal carries no
# conflict (heapam.c, HeapTupleHeaderAdvanceConflictHorizon, leaves out a row
# whose xmax is its xmin), so the snapshot survives a removal it could not
# have agreed to: a record deleted this way would be gone for a reader that
# still needed it.
$primary->safe_psql('postgres', q{
    BEGIN;
    INSERT INTO records VALUES ('other'::regclass, pg_current_xact_id()::xid);
    DELETE FROM records;
    COMMIT;
});
$primary->safe_psql('postgres', 'VACUUM records');
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
is($standby->safe_psql('postgres', q{SELECT pg_relation_size('records')}),
    0, 'deleted by its inserter: the row is physically removed on the standby');
is($idle->query_safe('SELECT count(*) FROM other'), 1,
    'deleted by its inserter: the older standby snapshot is not ended');

# A row deleted by a later transaction. Replaying its removal ends the
# snapshot, which here is idle in its transaction and so is terminated.
$primary->safe_psql('postgres', q{
    INSERT INTO records VALUES ('other'::regclass, pg_current_xact_id()::xid);
});
$primary->safe_psql('postgres', 'DELETE FROM records');
$primary->safe_psql('postgres', 'VACUUM records');
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
$standby->poll_query_until('postgres', q{
    SELECT confl_snapshot >= 2 FROM pg_stat_database_conflicts
    WHERE datname = 'postgres'
}) or die 'the idle standby snapshot was not ended';
is($standby->safe_psql('postgres', q{SELECT pg_relation_size('records')}),
    0, 'deleted by a later transaction: the row is removed on the standby');
is( $standby->safe_psql('postgres',
        "SELECT count(*) FROM pg_stat_activity WHERE pid = $idlePid"),
    0, 'deleted by a later transaction: the idle standby session is gone');
like(
    slurp_file($standby->logfile, $logOffset),
    qr/FATAL:  terminating connection due to conflict with recovery/,
    'deleted by a later transaction: the session was terminated by the conflict');
eval { $idle->quit; };

$standby->stop;
$primary->stop;

done_testing();
