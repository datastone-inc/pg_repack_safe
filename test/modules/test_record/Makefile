# Test-only module for the tests that need a record with a chosen table and
# transaction ID (sql/reader_check.sql, specs/reader_cache.spec,
# t/006_record_xid8.pl); installed by the top-level make installcheck, not by
# make install.

MODULES = test_record

# PostgreSQL's release-build headers have inline functions whose parameters
# are used only under assertions, so unused parameters are not errors.
PG_CPPFLAGS = -Wall -Wextra -Wno-unused-parameter -Werror

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
