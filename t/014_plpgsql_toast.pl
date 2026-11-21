# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# Open question 6 (doc/research/fix-design.md): a PL/pgSQL procedure that
# commits inside a loop holds a row with a TOASTed value across
# REPACK (CONCURRENTLY), which swaps in a new TOAST relation and drops the
# old one (repack.c, swap by relation). The procedure then uses the value.
#
# Answered: the values stay correct. In a procedure that may commit,
# PL/pgSQL detoasts a value before storing it in a variable, since a TOAST
# pointer can go stale after a commit (pl_exec.c:9009-9020), and the FOR
# loop's cursor, made holdable at COMMIT, detoasts everything it keeps
# (portalcmds.c:417-421). Nothing reads the old TOAST relation after the
# commit, so no reader check is involved.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use RepackSafeTest;
use IPC::Run;

my $node = preloadedNode('main');
$node->start;
$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_repack_safe;
    CREATE TABLE tt (id int PRIMARY KEY, big text);
    ALTER TABLE tt ALTER COLUMN big SET STORAGE EXTERNAL;
    INSERT INTO tt SELECT g, repeat(chr(64 + g), 100000) FROM generate_series(1, 3) g;
    CREATE PROCEDURE probe() LANGUAGE plpgsql AS $$
    DECLARE
        r record;
    BEGIN
        FOR r IN SELECT id, big FROM tt ORDER BY id LOOP
            COMMIT;
            PERFORM pg_advisory_lock(1);
            PERFORM pg_advisory_unlock(1);
            RAISE NOTICE 'row % length % first %', r.id, length(r.big), left(r.big, 1);
        END LOOP;
    END $$;
});

my $toastBefore = $node->safe_psql('postgres',
    q{SELECT reltoastrelid FROM pg_class WHERE oid = 'tt'::regclass});

my $holder = $node->background_psql('postgres');
$holder->query_safe('SELECT pg_advisory_lock(1)');

my ($out, $err) = ('', '');
my $call = IPC::Run::start(
    [ 'psql', '-XAtq', '-d', $node->connstr('postgres'), '-c', 'CALL probe()' ],
    '>', \$out, '2>', \$err);
$node->poll_query_until('postgres', q{
    SELECT EXISTS (SELECT 1 FROM pg_locks
                   WHERE locktype = 'advisory' AND NOT granted)
}) or die 'procedure never waited';

$node->safe_psql('postgres', 'REPACK (CONCURRENTLY) tt');
$holder->query_safe('SELECT pg_advisory_unlock(1)');
$call->finish;

isnt($node->safe_psql('postgres',
        q{SELECT reltoastrelid FROM pg_class WHERE oid = 'tt'::regclass}),
    $toastBefore, 'REPACK replaced the TOAST relation while the procedure waited');
unlike($err, qr/ERROR/, 'the procedure completes without error');
my @rows = $err =~ /row (\d) length (\d+) first (\w)/g;
is_deeply(\@rows, [1, 100000, 'A', 2, 100000, 'B', 3, 100000, 'C'],
    'every value read after the REPACK is the original one');

$holder->quit;
$node->stop;
done_testing();
