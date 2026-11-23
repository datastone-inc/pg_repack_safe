# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# The optional pre-check of REPACK (CONCURRENTLY) with sessions that are
# running a statement, which isolationtester cannot hold open
# (doc/research/fix-design.md, step 11; specs/precheck.spec covers the
# rest).
#
# - A single long statement in its own transaction is not counted: it locked
#   every relation of its plan when it started.
# - A long statement after the first of its transaction is counted, even at
#   READ COMMITTED: the isolation level is not visible to the check, which
#   errs towards refusing (a documented over-refusal).
# - The threshold counts only transactions open at least that long, and the
#   refusal names the process.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;
use IPC::Run;
use Time::HiRes qw(sleep);

my $node = preloadedNode('main');
$node->start;
$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT generate_series(1, 5);
    CREATE TABLE other (id int);
});

# Run statements in one background psql session, each as its own message
# (one -c each), and wait until it runs pg_sleep.
sub sleeper
{
    my @statements = @_;
    my ($out, $err) = ('', '');
    my $h = IPC::Run::start(
        [ 'psql', '-XAtq', '-d', $node->connstr('postgres'),
          map { ('-c', $_) } @statements ],
        '>', \$out, '2>', \$err);
    $node->poll_query_until('postgres', q{
        SELECT EXISTS (SELECT 1 FROM pg_stat_activity
                       WHERE query LIKE '%pg_sleep(30)%'
                         AND state = 'active' AND pid <> pg_backend_pid())
    }) or die 'sleeper never started';
    return $h;
}

sub repack
{
    my ($setting) = @_;
    return $node->psql('postgres',
        "SET pg_repack_safe.precheck_snapshot_age = $setting;\n"
          . 'REPACK (CONCURRENTLY) t;');
}

my $refusal = qr/cannot execute REPACK \(CONCURRENTLY\) on table "t" while another session holds an older snapshot/;

# A single statement in its own transaction.
my $h = sleeper('SELECT count(*) FROM other, pg_sleep(30)');
my ($ret, $stdout, $stderr) = repack(0);
is($ret, 0, 'single long statement: not counted, REPACK runs');
$h->kill_kill;

# A statement after the first of its READ COMMITTED transaction.
$h = sleeper('BEGIN', 'SELECT 1', 'SELECT count(*) FROM other, pg_sleep(30)',
    'COMMIT');
($ret, $stdout, $stderr) = repack(0);
isnt($ret, 0, 'later statement of a transaction: counted, REPACK refused');
like($stderr, $refusal, 'later statement of a transaction: the refusal');
like($stderr, qr/DETAIL:  A session in this database holds a snapshot and no lock on the table, and would get a serialization failure/,
    'the refusal says why');
unlike($stderr, qr/Process \d+|started at/,
    'the refusal does not identify the session, which pg_stat_activity may hide from this user');

# The threshold: the same transaction counts once it is old enough.
($ret, $stdout, $stderr) = repack("'1h'");
is($ret, 0, 'transaction younger than the threshold: REPACK runs');
sleep 0.5;
($ret, $stdout, $stderr) = repack("'200ms'");
isnt($ret, 0, 'transaction older than the threshold: REPACK refused');
$h->kill_kill;

$node->stop;
done_testing();
