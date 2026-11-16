# Makefile for the pg_repack_safe PostgreSQL extension

EXTENSION = pg_repack_safe
MODULE_big = pg_repack_safe
OBJS = src/pg_repack_safe.o
DATA = pg_repack_safe--1.0.0.sql

# doc_examples: validates SQL examples from README.md and doc/*.md
REGRESS = doc_examples storage_change reader_check

# Concurrency cases: specs/*.spec, expected/*.out. These need a server with
# wal_level = replica or higher, which REPACK (CONCURRENTLY) requires.
ISOLATION = repack_concurrently_snapshot repack_concurrently_cursor

# t/*.pl: cases that need separate client programs (pg_dump). Each test
# creates its own temporary cluster from the installed binaries.
TAP_TESTS = 1

# Compiler flags
PG_CPPFLAGS = -Wall -Wextra -Werror

# Build configuration
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
