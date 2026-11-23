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
# Every such load warns, and the warning names session_preload_libraries
# when that is where the library is listed.
#
# Not tested: the warning naming local_preload_libraries. That setting loads
# only from $libdir/plugins (miscinit.c, load_libraries), and installing the
# library there for a test would let any user load it on that installation.
# sessionPreloadSetting checks both settings in the same loop.
#
# Known gap, pinned: background workers do not load
# session_preload_libraries, so with it in place of shared_preload_libraries
# a background worker never loads the library, and its
# REPACK (CONCURRENTLY) is not refused (test/modules/toplevel_worker).
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

# A call that loads the library warns once, not once for the load and once
# for the call.
($ret, $stdout, $stderr) = $node->psql('postgres', q{
    SELECT pg_repack_safe_status();
});
my @warnings = ($stderr =~ /WARNING:  pg_repack_safe is not in force/g);
is(scalar(@warnings), 1,
    'not preloaded: a status call that loads the library warns once');

# Every late load warns: it happens only on a server that does not preload
# the library, so on a correctly configured server it never fires.
($ret, $stdout, $stderr) = $node->psql('postgres', q{
    LOAD 'pg_repack_safe';
});
is($ret, 0, 'library loaded with LOAD: the load succeeds');
like($stderr,
    qr/WARNING:  pg_repack_safe is not in force: the server did not load it through shared_preload_libraries/,
    'library loaded with LOAD: the load warns');
like($stderr,
    qr/HINT:  Add pg_repack_safe to shared_preload_libraries and restart the server\./,
    'library loaded with LOAD: the hint names shared_preload_libraries');
unlike($stderr, qr/session_preload_libraries/,
    'library loaded with LOAD: the warning does not blame session_preload_libraries');

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

# Each connection loads the library at start, and the load warns. The
# detail names the setting the administrator used and says what it misses.
like($stderr,
    qr/WARNING:  pg_repack_safe is not in force: the server did not load it through shared_preload_libraries/,
    'library in session_preload_libraries: the connection warns');
like($stderr,
    qr/DETAIL:  pg_repack_safe is listed in session_preload_libraries, which background workers do not load/,
    'library in session_preload_libraries: the detail names the setting');
like($stderr,
    qr/HINT:  Move pg_repack_safe from session_preload_libraries to shared_preload_libraries and restart the server\./,
    'library in session_preload_libraries: the hint says what to change');

# Not tested: the same for local_preload_libraries (see the header).

# The status function gives the same cause.
($ret, $stdout, $stderr) = $node->psql('postgres', q{
    SELECT pg_repack_safe_status();
});
is($stdout, 'f',
    'library in session_preload_libraries: pg_repack_safe_status() returns false');
@warnings = ($stderr =~ /DETAIL:  pg_repack_safe is listed in session_preload_libraries/g);
is(scalar(@warnings), 2,
    'library in session_preload_libraries: the connection and the status call both name the setting');

# Known gap, pinned: session_preload_libraries reaches client backends only
# (postinit.c, InitPostgres, INIT_PG_LOAD_SESSION_LIBS, which only
# PostgresMain passes). A background worker, the way schedulers such as
# pg_cron can run SQL, never loads the library: _PG_init never runs there,
# no hook is installed, and its REPACK (CONCURRENTLY) runs unrecorded, while
# each client session, which did load the library, refuses the same command.
# The preloaded case further down is the control: the same worker's
# REPACK (CONCURRENTLY) is recorded there.
$node->safe_psql('postgres', q{
    CREATE FUNCTION test_run_in_worker(text) RETURNS text
    AS 'toplevel_worker' LANGUAGE C STRICT;
});
my $filenodeBefore = $node->safe_psql('postgres',
    q{SELECT pg_relation_filenode('t')});
is($node->safe_psql('postgres',
        q{SELECT coalesce(test_run_in_worker('REPACK (CONCURRENTLY) t'),
                          'completed')}),
    'completed',
    'KNOWN GAP: session_preload_libraries, a background worker runs REPACK (CONCURRENTLY)');
isnt($node->safe_psql('postgres', q{SELECT pg_relation_filenode('t')}),
    $filenodeBefore,
    'KNOWN GAP: session_preload_libraries, the worker replaced the storage');
is($node->safe_psql('postgres',
        'SELECT count(*) FROM pg_repack_safe_rewrites'),
    '0',
    'KNOWN GAP: session_preload_libraries, the worker wrote no record');

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

# Control for the background worker case above: with the library preloaded,
# a background worker has the hooks (it is forked from the postmaster), and
# its REPACK (CONCURRENTLY) is recorded like a client's.
{
    my $preloaded = PostgreSQL::Test::Cluster->new('preloaded_worker');
    $preloaded->init(allows_streaming => 1);
    $preloaded->append_conf('postgresql.conf',
        "shared_preload_libraries = 'pg_repack_safe'\n");
    $preloaded->start;
    $preloaded->safe_psql('postgres', q{
        CREATE EXTENSION pg_repack_safe;
        CREATE TABLE t (id int PRIMARY KEY);
        INSERT INTO t SELECT generate_series(1, 5);
        CREATE FUNCTION test_run_in_worker(text) RETURNS text
        AS 'toplevel_worker' LANGUAGE C STRICT;
    });
    is($preloaded->safe_psql('postgres',
            q{SELECT coalesce(test_run_in_worker('REPACK (CONCURRENTLY) t'),
                              'completed')}),
        'completed',
        'preloaded: a background worker runs REPACK (CONCURRENTLY)');
    is($preloaded->safe_psql('postgres',
            q{SELECT count(*) > 0 FROM pg_repack_safe_rewrites
              WHERE relid = 't'::regclass}),
        't', 'preloaded: the background worker\'s REPACK (CONCURRENTLY) is recorded');
    $preloaded->stop;
}

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
