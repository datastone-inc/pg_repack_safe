# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Helpers shared by the TAP tests in this directory. A test loads them with
#
#     use FindBin;
#     use lib $FindBin::RealBin;
#     use RepackSafeTest;

package RepackSafeTest;

use strict;
use warnings FATAL => 'all';

use Exporter 'import';
use PostgreSQL::Test::Cluster;

our @EXPORT = qw(preloadedNode oldSnapshot runIn waitForLockRequest);

# Create and configure a cluster with libraries preloaded, without starting
# it. Options:
#   libraries         value of shared_preload_libraries
#                     (default 'pg_repack_safe')
#   allows_streaming  passed to init; default 1, because
#                     REPACK (CONCURRENTLY) needs wal_level = replica or
#                     higher
#   conf              further postgresql.conf lines
sub preloadedNode
{
    my ($name, %opts) = @_;
    my $libraries = $opts{libraries} // 'pg_repack_safe';
    my $streaming = $opts{allows_streaming} // 1;
    my $conf = $opts{conf} // '';

    my $node = PostgreSQL::Test::Cluster->new($name);
    $node->init(allows_streaming => $streaming);
    $node->append_conf('postgresql.conf',
        "shared_preload_libraries = '$libraries'\n$conf");
    return $node;
}

# Open a background session holding an old snapshot: a REPEATABLE READ
# transaction that has run one query, on a table the test does not repack,
# so it holds a snapshot and no lock on the tables read later. Errors do not
# end the session. Options:
#   table  the table the query reads (default 'other'); it must exist
#   user   role to connect as
sub oldSnapshot
{
    my ($node, %opts) = @_;
    my $table = $opts{table} // 'other';
    my $connstr = $node->connstr('postgres');
    $connstr .= " user=$opts{user}" if defined $opts{user};

    my $session = $node->background_psql('postgres',
        on_error_stop => 0, connstr => $connstr);
    $session->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
    $session->query_safe("SELECT count(*) FROM $table");
    return $session;
}

# Run one statement in a background session and return (stdout, stderr).
# The session's stderr buffer, which BackgroundPsql keeps across queries, is
# empty before the statement runs and again on return, so the result holds
# this statement's errors only and an expected error does not fail a later
# query_safe.
sub runIn
{
    my ($session, $sql) = @_;
    $session->{stderr} = '';
    my $out = $session->query($sql);
    my $err = $session->{stderr};
    $session->{stderr} = '';
    return ($out, $err);
}

# Wait until some session has requested, and not been granted, a lock of
# the given mode (as pg_locks spells it, such as 'AccessExclusiveLock') on
# a table.
sub waitForLockRequest
{
    my ($node, $table, $mode) = @_;
    $node->poll_query_until('postgres', qq{
        SELECT EXISTS (SELECT 1 FROM pg_locks
                       WHERE relation = '$table'::regclass
                         AND mode = '$mode' AND NOT granted)
    }) or die "no session queued for $mode on $table";
    return;
}

1;
