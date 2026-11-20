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
REGRESS = doc_examples storage_change reader_check writer protection
REGRESS_OPTS = --temp-instance=./tmp_check_regress \
               --temp-config=$(srcdir)/test/temp_instance.conf

# Concurrency cases: specs/*.spec, expected/*.out. They run on a temporary
# instance set up like the REGRESS one (test/temp_instance.conf).
ISOLATION = repack_concurrently_snapshot repack_concurrently_cursor \
            reader_guard_edges reader_cache precheck known_gaps \
            copy_attach_race
ISOLATION_OPTS = --temp-instance=./tmp_check_iso \
                 --temp-config=$(srcdir)/test/temp_instance.conf

# t/*.pl: cases that need separate client programs (pg_dump). Each test
# creates its own temporary cluster from the installed binaries.
TAP_TESTS = 1

# Compiler flags
# PostgreSQL's release-build headers have inline functions whose parameters
# are used only under assertions, so unused parameters are not errors.
PG_CPPFLAGS = -Wall -Wextra -Wno-unused-parameter -Werror

# Build configuration
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# The TAP tests load a test-only module (test/modules/oat_observer) next to
# this one; install it for them. make install does not install it.
installcheck: install-test-modules
install-test-modules:
	$(MAKE) -C test/modules/oat_observer PG_CONFIG=$(PG_CONFIG) install
.PHONY: install-test-modules
