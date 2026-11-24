# Makefile for the pg_repack_safe PostgreSQL extension

EXTENSION = pg_repack_safe
MODULE_big = pg_repack_safe
OBJS = src/pg_repack_safe.o
DATA = pg_repack_safe--1.0.sql

# Single-session cases: sql/*.sql, expected/*.out. doc_examples validates
# the SQL examples from README.md and doc/*.md. They run on a temporary
# instance built from the installed binaries, with the settings in
# test/temp_instance.conf (the library preloaded, wal_level = replica), and
# never on the server PGHOST/PGPORT point at.
REGRESS = doc_examples storage_change reader_check writer protection \
          copy_partitioned records_prune status
REGRESS_OPTS = --temp-instance=./tmp_check_regress \
               --temp-config=$(srcdir)/test/temp_instance.conf \
               --temp-config=$(TEST_MODULES_CONF)

# Concurrency cases: specs/*.spec, expected/*.out. They run on a temporary
# instance set up like the REGRESS one (test/temp_instance.conf).
ISOLATION = repack_concurrently_snapshot repack_concurrently_cursor \
            reader_guard_edges reader_cache precheck stock_rewrites \
            copy_attach_race record_prune unread_relations
ISOLATION_OPTS = --temp-instance=./tmp_check_iso \
                 --temp-config=$(srcdir)/test/temp_instance.conf \
                 --temp-config=$(TEST_MODULES_CONF)

# t/*.pl: cases a spec cannot set up: client programs (pg_dump, pg_upgrade),
# standbys, logical replication, and other preloaded modules. Each test
# creates its own temporary clusters from the installed binaries.
TAP_TESTS = 1

# Compiler flags
# PostgreSQL's release-build headers have inline functions whose parameters
# are used only under assertions, so unused parameters are not errors.
PG_CFLAGS = -Wall -Wextra -Wno-unused-parameter
# Warnings are errors only on request (make WERROR=1), so that a compiler
# with warnings this code has not met still builds it.
ifdef WERROR
PG_CFLAGS += -Werror
endif

# The test-only modules under test/modules are built for the tests and
# loaded from where they are built: the temporary servers get a
# dynamic_library_path that names those directories, in a generated
# configuration file. Nothing of them is installed.
TEST_MODULES = custom_scan_provider oat_observer test_record toplevel_worker
TEST_MODULES_CONF = $(CURDIR)/test/modules/library_path.conf
empty :=
space := $(empty) $(empty)
TEST_MODULES_PATH = $(subst $(space),:,$(addprefix \
                        $(CURDIR)/test/modules/,$(TEST_MODULES)))
# A TEMP_CONFIG from the environment, read here because installcheck sets
# its own for itself and its prerequisites. The generated file includes it,
# so its settings reach every temporary server.
USER_TEMP_CONFIG := $(filter-out $(TEST_MODULES_CONF),$(abspath $(TEMP_CONFIG)))

# Build configuration
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# The test-only modules:
# - custom_scan_provider: plans table scans as a CustomScan, for
#   specs/unread_relations.spec.
# - oat_observer: a second object_access_hook, loaded next to this library
#   by t/015_oat_hook_chain.pl.
# - test_record: writes a record with a chosen table and transaction ID, for
#   the regress, isolation and TAP cases that need a record no REPACK
#   (CONCURRENTLY) produces.
# - toplevel_worker: runs a statement at top level in a background worker,
#   for t/009_not_preloaded.pl.
# pg_regress and pg_isolation_regress take the generated file as a second
# --temp-config; the TAP tests' clusters append the file TEMP_CONFIG names
# (PostgreSQL::Test::Cluster, init). Each appends the file's text to
# postgresql.conf, where an include of the developer's TEMP_CONFIG works.
installcheck: build-test-modules
installcheck: export TEMP_CONFIG = $(TEST_MODULES_CONF)
build-test-modules:
	for m in $(TEST_MODULES); do \
	    $(MAKE) -C test/modules/$$m PG_CONFIG=$(PG_CONFIG) all || exit 1; \
	done
	printf "dynamic_library_path = '%s:\$$libdir'\n" \
	    "$(TEST_MODULES_PATH)" > $(TEST_MODULES_CONF)
	$(if $(USER_TEMP_CONFIG),printf "include '%s'\n" \
	    "$(USER_TEMP_CONFIG)" >> $(TEST_MODULES_CONF))
clean: clean-test-modules
clean-test-modules:
	for m in $(TEST_MODULES); do \
	    $(MAKE) -C test/modules/$$m PG_CONFIG=$(PG_CONFIG) clean; \
	done
	rm -f $(TEST_MODULES_CONF)
.PHONY: build-test-modules clean-test-modules

# Release. The library's version is the constant in the C file, which
# pg_get_loaded_modules() reports; META.json carries the same version for
# PGXN, at the top and in its provides entry, and the control file's
# default_version is its first two parts.
# check-version fails if they disagree. dist writes the PGXN archive of the
# committed tree, so uncommitted changes are not in it.
LIBRARY_VERSION = $(shell sed -n \
    's/^\#define PG_REPACK_SAFE_VERSION "\(.*\)"/\1/p' src/pg_repack_safe.c)
check-version:
	@test -n "$(LIBRARY_VERSION)" || \
	    { echo "no PG_REPACK_SAFE_VERSION in src/pg_repack_safe.c"; exit 1; }
	@grep -q '^   "version": "$(LIBRARY_VERSION)",$$' META.json || \
	    { echo "META.json is not version $(LIBRARY_VERSION)"; exit 1; }
	@grep -q '^         "version": "$(LIBRARY_VERSION)"$$' META.json || \
	    { echo "META.json provides another version than $(LIBRARY_VERSION)"; \
	    exit 1; }
	@v=$$(sed -n "s/^default_version = '\(.*\)'/\1/p" \
	    pg_repack_safe.control); \
	    case "$(LIBRARY_VERSION)" in "$$v".*) ;; *) \
	    echo "default_version $$v is not a prefix of $(LIBRARY_VERSION)"; \
	    exit 1;; esac; \
	    test -f "pg_repack_safe--$$v.sql" || \
	    { echo "no install script for version $$v"; exit 1; }
	@echo "version $(LIBRARY_VERSION): consistent"
dist: check-version
	git archive --format zip \
	    --prefix=pg_repack_safe-$(LIBRARY_VERSION)/ \
	    -o pg_repack_safe-$(LIBRARY_VERSION).zip HEAD
.PHONY: check-version dist

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
