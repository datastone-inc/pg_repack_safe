# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Design assumption behind the unsafe-rewrite record table, on a hot standby
# without hot_standby_feedback (doc/research/fix-design.md, section 6c,
# "Record lifetime").
#
# The primary cannot see standby snapshots, so it may DELETE and VACUUM a
# record while a standby transaction still needs it. The reader check scans
# the record table with SnapshotAny, so the design holds only if both of
# these are true:
#
#   1. After the DELETE is replayed, the row is still physically present on
#      the standby, although no MVCC snapshot taken now sees it.
#   2. The row cannot be physically removed on the standby while a snapshot
#      that predates the row's inserter is alive: replaying the VACUUM
#      cancels that snapshot's query instead.
#
# This test uses a plain table with the record table's shape; it does not
# load the extension.

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

$standby->stop;
$primary->stop;

done_testing();
