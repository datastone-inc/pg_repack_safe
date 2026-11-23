# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# object_access_hook chaining with another module that installs it
# (doc/research/fix-design.md, step 13 hook audit).
#
# test/modules/oat_observer reports every OAT_DROP as a NOTICE before calling
# the previous hook. With both libraries preloaded, in either order, a DROP
# that pg_repack_safe refuses must show the observer's NOTICE and the
# refusal (both hooks ran), and an unprotected DROP must show the NOTICE and
# succeed.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;

my %orders = (
    observer_first => 'oat_observer, pg_repack_safe',
    observer_last => 'pg_repack_safe, oat_observer');

foreach my $name (sort keys %orders)
{
    my $node = preloadedNode($name, libraries => $orders{$name});
    $node->start;
    $node->safe_psql('postgres', q{
        CREATE EXTENSION pg_repack_safe;
        CREATE TABLE t (id int PRIMARY KEY);
        INSERT INTO t SELECT generate_series(1, 5);
        REPACK (CONCURRENTLY) t;
        CREATE TABLE plain (id int);
    });

    my ($ret, $stdout, $stderr) =
      $node->psql('postgres', 'DROP EXTENSION pg_repack_safe');
    isnt($ret, 0, "$name: protected drop refused");
    like($stderr, qr/NOTICE:  oat_observer: drop of /,
        "$name: the observer's hook ran");
    like($stderr,
        qr/cannot drop the pg_repack_safe extension, its record table or its triggers/,
        "$name: pg_repack_safe's hook ran");

    ($ret, $stdout, $stderr) = $node->psql('postgres', 'DROP TABLE plain');
    is($ret, 0, "$name: unprotected drop succeeds");
    like($stderr, qr/NOTICE:  oat_observer: drop of table plain/,
        "$name: the observer saw it");

    $node->stop;
}

done_testing();
