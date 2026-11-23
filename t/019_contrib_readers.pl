# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Known gap (README "Contrib functions the guard does not see"): functions
# of two contrib modules read a table with the transaction's snapshot
# outside the executor, so the reader check does not see the read, and with
# a snapshot that predates a REPACK (CONCURRENTLY) of the table they return
# a wrong answer with no error.
#
# - pgrowlocks() opens the table named by its argument and scans it with the
#   active snapshot (contrib/pgrowlocks/pgrowlocks.c, pgrowlocks). The old
#   snapshot gets no row locks for a table that has a locked row.
# - bt_index_check(index, heapallindexed => true) registers the transaction
#   snapshot and scans the heap with it (contrib/amcheck/verify_nbtree.c,
#   bt_check_every_level). The old snapshot sees no heap tuple, so the
#   function has nothing to look up in the index and reports success.
# - bt_index_check and bt_index_parent_check with checkunique register the
#   transaction snapshot and fetch the heap tuple of each index entry with
#   it, to compare only entries of visible rows
#   (contrib/amcheck/verify_nbtree.c, heap_entry_is_visible). The old
#   snapshot sees no heap tuple, so no two entries are compared and an index
#   with duplicates passes. The test gets such an index by marking a
#   non-unique index unique in pg_index after the REPACK.
#
# The test pins that behaviour, and that a query of the same table with the
# same snapshot gets the reader guard's error.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;

my $node = preloadedNode('contrib_readers');
$node->start;
$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE EXTENSION pgrowlocks;
    CREATE EXTENSION amcheck;
    CREATE TABLE t (id int PRIMARY KEY, v int NOT NULL);
    INSERT INTO t SELECT g, 1 FROM generate_series(1, 5) g;
    CREATE INDEX t_v ON t (v);
    CREATE TABLE other (id int);
});

my $reader = oldSnapshot($node);
$node->safe_psql('postgres', 'REPACK (CONCURRENTLY) t');

# Another session locks one row of the repacked table.
my $locker = $node->background_psql('postgres');
$locker->query_safe('BEGIN');
$locker->query_safe('SELECT id FROM t WHERE id = 1 FOR UPDATE');

is( $node->safe_psql('postgres', q{SELECT count(*) FROM pgrowlocks('t')}),
    '1', 'a new snapshot sees the row lock');

my ($out, $err) = runIn($reader, q{SELECT count(*) FROM pgrowlocks('t')});
is($err, '', 'pgrowlocks with the old snapshot raises no error');
is($out, '0', 'pgrowlocks with the old snapshot sees 0 of 1 row locks');

($out, $err) = runIn($reader, q{SELECT bt_index_check('t_pkey', true)});
is($err, '',
    'bt_index_check with heapallindexed and the old snapshot reports success');

# t_v holds five entries with the same key; marked unique, it is an index
# that violates its constraint.
$node->safe_psql('postgres', q{
    UPDATE pg_index SET indisunique = true
    WHERE indexrelid = 't_v'::regclass});
my ($ret, $stdout, $stderr) =
  $node->psql('postgres', q{SELECT bt_index_check('t_v', false, true)});
like(
    $stderr,
    qr/index uniqueness is violated for index "t_v"/,
    'bt_index_check with checkunique and a new snapshot finds the duplicates'
);
($out, $err) = runIn($reader, q{SELECT bt_index_check('t_v', false, true)});
is($err, '',
    'bt_index_check with checkunique and the old snapshot reports success');
($out, $err) =
  runIn($reader, q{SELECT bt_index_parent_check('t_v', false, false, true)});
is($err, '',
    'bt_index_parent_check with checkunique and the old snapshot reports success'
);

($out, $err) = runIn($reader, 'SELECT count(*) FROM t');
like(
    $err,
    qr/cannot read table "t" with this snapshot/,
    'a query of the table with the old snapshot is refused');

$locker->query_safe('ROLLBACK');
$locker->quit;
$reader->quit;
$node->stop;
done_testing();
