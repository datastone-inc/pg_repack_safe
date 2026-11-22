# Copyright (c) 2026 dataStone Inc.
# SPDX-License-Identifier: MIT
#
# demo/demo.sh shows what the README says it shows.
#
# The demo runs four scenarios on a throwaway server, first without the
# library and then with the extension. This test runs it and checks both
# halves of its output:
# - on stock PostgreSQL the report counts 0 orders, the UPDATE changes 0
#   rows, the DELETE removes 0 rows and leaves orders without a customer,
#   and pg_dump exits 0 with no rows in the dump, each without an error;
# - with the extension each of the four gets the reader's error instead,
#   and none of the wrong results appears.
# So the demo cannot drift from the behaviour, and the stock half pins the
# wrong results this extension exists for.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use FindBin;

my $demo = "$FindBin::RealBin/../demo/demo.sh";
local $ENV{DEMO_PORT} = PostgreSQL::Test::Cluster::get_free_port();

my ($out, $err) = ('', '');
my $ok = IPC::Run::run([$demo], '>&', \$out);
ok($ok, 'the demo runs to the end') or diag($out);

my ($stock, $guarded) = split /^PostgreSQL \S+ with pg_repack_safe.*$/m, $out;
ok(defined $guarded, 'the output has a stock half and an extension half');
$guarded //= '';

my $readerError = qr/ERROR:  cannot read table "orders" with this snapshot/;

# Stock: wrong results, and nothing says so.
like($stock, qr/^ +0 \| +$/m, 'stock: the report counts 0 orders');
like($stock, qr/^UPDATE 0$/m, 'stock: the UPDATE changes 0 rows');
like($stock, qr/^DELETE 0\n.*^DELETE 1$/ms,
    'stock: the DELETE removes 0 orders, and the customer is deleted');
like($stock, qr/^ +0 \| +10$/m,
    'stock: ten orders are left without their customer');
like($stock, qr/^pg_dump exit status: 0\nrows in the dump: 0$/m,
    'stock: pg_dump succeeds with no rows in the dump');
unlike($stock, qr/ERROR/, 'stock: no error is raised');

# With the extension: the error, four times, and no wrong result.
my @errors = $guarded =~ /$readerError/g;
is(scalar @errors, 4, 'extension: each scenario gets the error');
like($guarded, qr/^pg_dump exit status: 1$/m, 'extension: pg_dump fails');
unlike($guarded, qr/^ +0 \| +$/m, 'extension: the report counts no orders as 0');
unlike($guarded, qr/^UPDATE 0$/m, 'extension: no UPDATE of 0 rows');
unlike($guarded, qr/^DELETE \d+$/m, 'extension: nothing is deleted');
like($guarded, qr/^ +1 \| +10$/m,
    'extension: the customer and its ten orders are still there');

# In both halves a new transaction sees every order.
my @totals = $out =~ /^ +1000 \| 10000\.00$/mg;
is(scalar @totals, 2, 'a new transaction reads all 1000 orders in both halves');

done_testing();
