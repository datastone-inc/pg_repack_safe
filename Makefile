# Makefile for the pg_repack_safe PostgreSQL extension

EXTENSION = pg_repack_safe
MODULE_big = pg_repack_safe
OBJS = src/pg_repack_safe.o
DATA = pg_repack_safe--1.0.0.sql

# Single-session cases: sql/*.sql, expected/*.out. doc_examples validates
# the SQL examples from README.md and doc/*.md. They run on a temporary
# instance built from the installed binaries, with the settings in
# test/temp_instance.conf (the library preloaded, wal_level = replica), and
# never on the server PGHOST/PGPORT point at.
REGRESS = doc_examples storage_change reader_check writer protection \
          copy_partitioned records_prune status
REGRESS_OPTS = --temp-instance=./tmp_check_regress \
               --temp-config=$(srcdir)/test/temp_instance.conf

# Concurrency cases: specs/*.spec, expected/*.out. They run on a temporary
# instance set up like the REGRESS one (test/temp_instance.conf).
ISOLATION = repack_concurrently_snapshot repack_concurrently_cursor \
            reader_guard_edges reader_cache precheck stock_rewrites \
            copy_attach_race record_prune
ISOLATION_OPTS = --temp-instance=./tmp_check_iso \
                 --temp-config=$(srcdir)/test/temp_instance.conf

# t/*.pl: cases a spec cannot set up: client programs (pg_dump, pg_upgrade),
# standbys, logical replication, and other preloaded modules. Each test creates its own temporary clusters from the
# installed binaries.
TAP_TESTS = 1

# Compiler flags
# PostgreSQL's release-build headers have inline functions whose parameters
# are used only under assertions, so unused parameters are not errors.
PG_CPPFLAGS = -Wall -Wextra -Wno-unused-parameter
# Warnings are errors only on request (make WERROR=1), so that a compiler
# with warnings this code has not met still builds it.
ifdef WERROR
PG_CPPFLAGS += -Werror
endif

# Build configuration
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# The tests use test-only modules; install them for the tests. make install
# does not install them.
# - test/modules/oat_observer: a second object_access_hook, loaded next to
#   this library by t/015_oat_hook_chain.pl.
# - test/modules/test_record: writes a record with a chosen table and
#   transaction ID, for the regress, isolation and TAP cases that need a
#   record no REPACK (CONCURRENTLY) produces.
# - test/modules/toplevel_worker: runs a statement at top level in a
#   background worker, for t/009_not_preloaded.pl.
installcheck: install-test-modules
install-test-modules:
	$(MAKE) -C test/modules/oat_observer PG_CONFIG=$(PG_CONFIG) install
	$(MAKE) -C test/modules/test_record PG_CONFIG=$(PG_CONFIG) install
	$(MAKE) -C test/modules/toplevel_worker PG_CONFIG=$(PG_CONFIG) install
.PHONY: install-test-modules

# C formatting, by the rules in .clang-format. check-format changes nothing
# and fails if a file is not formatted; format rewrites the files in place.
# Output differs between clang-format majors, so the CI names its binary
# (make check-format CLANG_FORMAT=clang-format-22).
CLANG_FORMAT ?= clang-format
FORMAT_SOURCES = $(wildcard src/*.c src/*.h test/modules/*/*.c \
                            test/modules/*/*.h)
format:
	$(CLANG_FORMAT) -i $(FORMAT_SOURCES)
check-format:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SOURCES)
.PHONY: format check-format
