# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The guard under debug_discard_caches = 1.
#
# With that setting every AcceptInvalidationMessages call runs
# InvalidateSystemCaches (inval.c, AcceptInvalidationMessages), which calls
# every registered syscache and relcache callback although no invalidation
# message arrived. The lookups of the record table and the load of the
# record cache repeat when a message arrived while they ran; they tell that
# from SharedInvalidMessageCounter, which the debug discard does not advance
# (invalidatedSince), so they end. A loop that repeated whenever a callback
# fired never ended, and every query in a database with the extension hung.
#
# The reader check, the writer and repack_safe.prune_records() each run
# here with the setting on, and the guard still refuses an old snapshot.
#
# debug_discard_caches can be set only in a build with assertions; the test
# is skipped elsewhere.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;

my $node = preloadedNode('discard');
$node->start;

if ($node->safe_psql('postgres', 'SHOW debug_assertions') ne 'on')
{
    $node->stop;
    plan skip_all => 'debug_discard_caches needs a build with assertions';
}

$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
    CREATE TABLE other (id int);
});

# A statement that hangs is cancelled, and the test fails on the error.
my $settings = q{
    SET statement_timeout = '60s';
    SET debug_discard_caches = 1;
};

# Reader check, no record yet.
my ($ret, $stdout, $stderr) =
  $node->psql('postgres', "$settings SELECT count(*) FROM t");
is($ret, 0, 'a query finishes') or diag($stderr);
is($stdout, '5', 'and returns its rows');

# An old snapshot whose session discards its caches on every lookup.
my $reader = oldSnapshot($node);
$reader->query_safe($settings);

# Writer: lock the record table, record the rewrite.
($ret, $stdout, $stderr) =
  $node->psql('postgres', "$settings REPACK (CONCURRENTLY) t");
is($ret, 0, 'REPACK (CONCURRENTLY) finishes') or diag($stderr);
is( $node->safe_psql('postgres',
        q{SELECT count(*) FROM repack_safe.rewrites
          WHERE relid = 't'::regclass}),
    '1', 'and writes its record');

# Reader check with a record: load the record cache, refuse.
my ($out, $err) = runIn($reader, 'SELECT count(*) FROM t');
like(
    $err,
    qr/cannot read table "t" with this snapshot/,
    'the old snapshot is refused');
$reader->query('ROLLBACK');

# A new snapshot in the same session reads the table.
($out, $err) = runIn($reader, 'SELECT count(*) FROM t');
is($err, '', 'a new snapshot is not refused');
is($out, '5', 'and reads every row');

# The SQL function that locks the record table.
($ret, $stdout, $stderr) = $node->psql('postgres',
    "$settings SELECT repack_safe.prune_records()");
is($ret, 0, 'repack_safe.prune_records() finishes') or diag($stderr);

$reader->quit;
$node->stop;
done_testing();
