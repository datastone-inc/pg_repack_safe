# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# pg_upgrade with the library preloaded in the old and the new cluster.
#
# In binary-upgrade mode pg_dump creates the extension empty and adds its
# record table in a later statement, with queries in between. The reader
# check must not refuse those queries, or pg_upgrade cannot restore a
# database that has the extension. After the upgrade the check must be in
# force again, and the old cluster's records, deleted ones included, must
# have come along unchanged.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;

# REPACK (CONCURRENTLY) needs wal_level = replica or higher.
my $old = preloadedNode('old');
$old->start;
$old->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE customers (id int PRIMARY KEY, name text NOT NULL);
    INSERT INTO customers SELECT g, 'customer ' || g FROM generate_series(1, 5) g;
    REPACK (CONCURRENTLY) customers USING INDEX customers_pkey;
    REPACK (CONCURRENTLY) customers USING INDEX customers_pkey;
    CREATE EXTENSION pageinspect;
});
# Row versions physically in the record table: the second REPACK deleted the
# first one's record, and nothing has vacuumed it.
my $versionsQuery = q{
    SELECT count(*) FROM heap_page_items(
        get_raw_page('repack_safe.rewrites', 0))
    WHERE lp_len > 0};
my $oldVersions = $old->safe_psql('postgres', $versionsQuery);
my $recordsQuery = q{
    SELECT relid::regclass, rewrite_xid FROM repack_safe.rewrites
    ORDER BY relid::regclass::text};
my $oldRecords = $old->safe_psql('postgres', $recordsQuery);
$old->stop;

my $new = preloadedNode('new');

my $bindir = $new->config_data('--bindir');

# pg_upgrade writes its scripts into the current directory.
chdir $PostgreSQL::Test::Utils::tmp_check;

command_ok(
    [
        'pg_upgrade', '--no-sync',
        '--old-datadir' => $old->data_dir,
        '--new-datadir' => $new->data_dir,
        '--old-bindir' => $bindir,
        '--new-bindir' => $bindir,
        '--socketdir' => $new->host,
        '--old-port' => $old->port,
        '--new-port' => $new->port,
    ],
    'pg_upgrade succeeds with the library preloaded and the extension created');

$new->start;

is( $new->safe_psql('postgres', 'SHOW shared_preload_libraries'),
    'pg_repack_safe',
    'library is preloaded in the upgraded cluster');

is( $new->safe_psql('postgres', q{
        SELECT n.nspname || '.' || c.relname
        FROM pg_extension e
        JOIN pg_depend d ON d.refclassid = 'pg_extension'::regclass
                        AND d.refobjid = e.oid
                        AND d.classid = 'pg_class'::regclass
                        AND d.deptype = 'e'
        JOIN pg_class c ON c.oid = d.objid
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE e.extname = 'pg_repack_safe'}),
    'repack_safe.rewrites',
    'record table belongs to the extension after the upgrade');

# The records come along: pg_upgrade keeps relation OIDs and transaction
# IDs, so each still names the relation and the rewrite it was written for.
# No snapshot from before the upgrade exists, so they refuse nothing.
isnt($oldRecords, '', 'the old cluster holds records');
is($new->safe_psql('postgres', $recordsQuery),
    $oldRecords, 'records are carried over unchanged');
is($new->safe_psql('postgres', 'SELECT count(*) FROM customers'),
    '5', 'user data survives the upgrade, and a recorded table can be read');

# The deleted record comes along too: pg_upgrade copies the table's file.
# Nothing in the new cluster needs it, and VACUUM removes it there.
my $liveRecords = $new->safe_psql('postgres',
    'SELECT count(*) FROM repack_safe.rewrites');
cmp_ok($oldVersions, '>', $liveRecords,
    'the old cluster held a deleted record');
is($new->safe_psql('postgres', $versionsQuery),
    $oldVersions, 'deleted records are carried over as well');
$new->safe_psql('postgres', 'VACUUM repack_safe.rewrites');
is($new->safe_psql('postgres', $versionsQuery),
    $liveRecords, 'VACUUM removes the deleted record in the new cluster');
is($new->safe_psql('postgres', 'SELECT repack_safe.prune_records()'),
    $liveRecords, 'the remaining records can be deleted in the new cluster');

# Outside binary-upgrade mode the refusal is in force.
# The table still holds the deleted records' blocks, so renaming it needs the
# override (sql/protection.sql).
$new->safe_psql('postgres', q{
    SET pg_repack_safe.allow_unprotected_removal = on;
    ALTER TABLE repack_safe.rewrites RENAME TO renamed});
my ($ret, $stdout, $stderr) = $new->psql('postgres', 'SELECT 1');
isnt($ret, 0, 'query refused when the record table is missing');
like(
    $stderr,
    qr/record table "rewrites" of extension "pg_repack_safe" does not exist/,
    'refusal names the missing record table');

$new->stop;

# A server started by hand in binary-upgrade mode: the reader exemption
# applies there too, so REPACK (CONCURRENTLY) must be refused while the
# record table is missing.
system_or_bail(
    'pg_ctl', '--wait',
    '--pgdata' => $new->data_dir,
    '--log' => $new->logfile,
    '--options' => '-b',
    'start');
$new->_update_pid(1);

is($new->safe_psql('postgres', 'SELECT 1'),
    '1', 'query passes in binary-upgrade mode with the record table missing');
($ret, $stdout, $stderr) = $new->psql('postgres',
    'REPACK (CONCURRENTLY) customers USING INDEX customers_pkey');
isnt($ret, 0,
    'REPACK (CONCURRENTLY) refused in binary-upgrade mode with the record table missing');
like(
    $stderr,
    qr/record table "rewrites" of extension "pg_repack_safe" does not exist/,
    'REPACK refusal names the missing record table');
$new->safe_psql('postgres',
    'ALTER TABLE repack_safe.renamed RENAME TO rewrites');

# Stock PostgreSQL cannot create the new heap in binary-upgrade mode, so the
# positive case runs after a normal start. (pg_ctl restart would reuse -b.)
$new->stop;
$new->start;
$new->safe_psql('postgres',
    'REPACK (CONCURRENTLY) customers USING INDEX customers_pkey');
is($new->safe_psql('postgres', 'SELECT count(*) FROM customers'),
    '5', 'REPACK (CONCURRENTLY) runs once the record table is back');

$new->stop;

done_testing();
