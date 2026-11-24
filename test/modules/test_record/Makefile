# Test-only module for the tests that need a record with a chosen table and
# transaction ID (sql/reader_check.sql, specs/reader_cache.spec,
# t/006_record_xid8.pl); built by the top-level make installcheck and loaded
# from this directory, never installed.

MODULES = test_record

# PostgreSQL's release-build headers have inline functions whose parameters
# are used only under assertions, so unused parameters are not errors.
PG_CFLAGS = -Wall -Wextra -Wno-unused-parameter
# Warnings are errors only on request (make WERROR=1), so that a compiler
# with warnings this code has not met still builds it.
ifdef WERROR
PG_CFLAGS += -Werror
endif

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
