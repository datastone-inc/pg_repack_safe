# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The library loaded other than through shared_preload_libraries
# (doc/research/fix-design.md, step 9).
#
# The reader guard works only if every backend has the library loaded
# before its first query, which only shared_preload_libraries provides. A
# backend that loaded the library some other way (LOAD, the extension's C
# trigger function, session_preload_libraries) refuses REPACK (CONCURRENTLY):
# other sessions may not check their reads against its record.
#
# Known gap, pinned: a backend that never loads the library has no hooks,
# so its REPACK (CONCURRENTLY) runs unrecorded. REPACK does not fire event
# triggers (cmdtaglist.h, CMDTAG_REPACK), so the extension cannot make it
# load the library.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 1);
$node->start;
$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
});

my ($ret, $stdout, $stderr) = $node->psql('postgres', q{
    LOAD 'pg_repack_safe';
    REPACK (CONCURRENTLY) t;
});
isnt($ret, 0, 'library loaded with LOAD: REPACK (CONCURRENTLY) refused');
like($stderr,
    qr/REPACK \(CONCURRENTLY\) requires pg_repack_safe in shared_preload_libraries/,
    'library loaded with LOAD: the error names the setting');

# Firing a trigger of the record table loads the library for its C function.
# TRUNCATE fires the BEFORE statement trigger and raises no error: the
# table is empty.
($ret, $stdout, $stderr) = $node->psql('postgres', q{
    BEGIN;
    TRUNCATE pg_repack_safe_rewrites;
    ROLLBACK;
    REPACK (CONCURRENTLY) t;
});
isnt($ret, 0,
    'library loaded by a trigger function: REPACK (CONCURRENTLY) refused');
like($stderr,
    qr/REPACK \(CONCURRENTLY\) requires pg_repack_safe in shared_preload_libraries/,
    'library loaded by a trigger function: the error names the setting');

$node->append_conf('postgresql.conf',
    "session_preload_libraries = 'pg_repack_safe'\n");
$node->reload;
($ret, $stdout, $stderr) =
  $node->psql('postgres', 'REPACK (CONCURRENTLY) t');
isnt($ret, 0,
    'library in session_preload_libraries: REPACK (CONCURRENTLY) refused');
like($stderr,
    qr/REPACK \(CONCURRENTLY\) requires pg_repack_safe in shared_preload_libraries/,
    'library in session_preload_libraries: the error names the setting');
$node->append_conf('postgresql.conf', "session_preload_libraries = ''\n");
$node->reload;

# Known gap: a backend without the library cannot refuse or record.
($ret, $stdout, $stderr) =
  $node->psql('postgres', 'REPACK (CONCURRENTLY) t');
is($ret, 0, 'KNOWN GAP: library never loaded, REPACK (CONCURRENTLY) runs');
is($node->safe_psql('postgres',
        'SELECT count(*) FROM pg_repack_safe_rewrites'),
    '0', 'KNOWN GAP: library never loaded, no record is written');

$node->stop;
done_testing();
