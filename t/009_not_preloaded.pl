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

# The status function tells this server apart from one that preloads the
# library: it returns false, with a WARNING that says what is wrong. Calling
# it loads the library in this backend, which then refuses
# REPACK (CONCURRENTLY) like any late load.
my ($ret, $stdout, $stderr) = $node->psql('postgres', q{
    SELECT pg_repack_safe_status();
});
is($stdout, 'f', 'not preloaded: pg_repack_safe_status() returns false');
like($stderr,
    qr/WARNING:  pg_repack_safe is not in force: the server did not load it through shared_preload_libraries/,
    'not preloaded: the warning says what is wrong');
($ret, $stdout, $stderr) = $node->psql('postgres', q{
    SELECT pg_repack_safe_status();
    REPACK (CONCURRENTLY) t;
});
like($stderr,
    qr/REPACK \(CONCURRENTLY\) requires pg_repack_safe in shared_preload_libraries/,
    'not preloaded: after the status call, REPACK (CONCURRENTLY) is refused');

($ret, $stdout, $stderr) = $node->psql('postgres', q{
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

# Where the library cannot be installed, PostgreSQL itself can switch
# REPACK (CONCURRENTLY) off for the whole server (README, "Without the
# extension"): it needs a replication slot from a pool of its own, and
# refuses when that pool is empty. REPACK without CONCURRENTLY, which is
# MVCC-safe, does not use it.
$node->append_conf('postgresql.conf', "max_repack_replication_slots = 0\n");
$node->restart;
($ret, $stdout, $stderr) =
  $node->psql('postgres', 'REPACK (CONCURRENTLY) t');
isnt($ret, 0,
    'max_repack_replication_slots = 0: REPACK (CONCURRENTLY) is refused');
like($stderr,
    qr/REPACK can only be used if "max_repack_replication_slots" > 0/,
    'max_repack_replication_slots = 0: PostgreSQL names the setting');
($ret, $stdout, $stderr) = $node->psql('postgres', 'REPACK t');
is($ret, 0, 'max_repack_replication_slots = 0: plain REPACK still runs')
  or diag($stderr);

$node->stop;

# Known gap, pinned: the refusal to drop the extension while its record
# table holds records is in the library's object access hook, so a server
# that no longer preloads the library drops it anyway (README, "Removing the
# extension", step 5). The same drop on the same records is refused while
# the library is preloaded.
{
    my $unloaded = PostgreSQL::Test::Cluster->new('unloaded_drop');
    $unloaded->init(allows_streaming => 1);
    $unloaded->append_conf('postgresql.conf',
        "shared_preload_libraries = 'pg_repack_safe'\n");
    $unloaded->start;
    $unloaded->safe_psql('postgres', q{
        CREATE EXTENSION pg_repack_safe;
        CREATE TABLE t (id int PRIMARY KEY);
        INSERT INTO t SELECT generate_series(1, 5);
        REPACK (CONCURRENTLY) t;
    });
    is($unloaded->safe_psql('postgres',
            'SELECT count(*) > 0 FROM pg_repack_safe_rewrites'),
        't', 'preloaded: REPACK (CONCURRENTLY) wrote a record');
    ($ret, $stdout, $stderr) =
      $unloaded->psql('postgres', 'DROP EXTENSION pg_repack_safe');
    isnt($ret, 0, 'preloaded: DROP EXTENSION is refused while records exist');

    $unloaded->append_conf('postgresql.conf',
        "shared_preload_libraries = ''\n");
    $unloaded->restart;
    is($unloaded->safe_psql('postgres',
            'SELECT count(*) > 0 FROM pg_repack_safe_rewrites'),
        't', 'not preloaded: the record is still there');
    ($ret, $stdout, $stderr) =
      $unloaded->psql('postgres', 'DROP EXTENSION pg_repack_safe');
    is($ret, 0,
        'KNOWN GAP: not preloaded, DROP EXTENSION goes through with records')
      or diag($stderr);
    is($unloaded->safe_psql('postgres',
            q{SELECT count(*) FROM pg_extension WHERE extname = 'pg_repack_safe'}),
        '0', 'KNOWN GAP: not preloaded, the extension is gone');
    $unloaded->stop;
}

done_testing();
