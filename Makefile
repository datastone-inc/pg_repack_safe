# Makefile for the pg_repack_safe PostgreSQL extension

EXTENSION = pg_repack_safe
MODULE_big = pg_repack_safe
OBJS = src/pg_repack_safe.o
DATA = pg_repack_safe--1.0.0.sql

# doc_examples: validates SQL examples from README.md and doc/*.md
REGRESS = doc_examples storage_change reader_check writer

# Concurrency cases: specs/*.spec, expected/*.out. They run on a temporary
# instance built from the installed binaries, with the settings in
# specs/isolation.conf (the library preloaded, wal_level = replica), and
# never on the server PGHOST/PGPORT point at.
ISOLATION = repack_concurrently_snapshot repack_concurrently_cursor \
            reader_guard_edges
ISOLATION_OPTS = --temp-instance=./tmp_check_iso \
                 --temp-config=$(srcdir)/specs/isolation.conf

# t/*.pl: cases that need separate client programs (pg_dump). Each test
# creates its own temporary cluster from the installed binaries.
TAP_TESTS = 1

# Compiler flags
PG_CPPFLAGS = -Wall -Wextra -Werror

# Build configuration
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
