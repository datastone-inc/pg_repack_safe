# Test-only module for t/009_not_preloaded.pl: runs one statement at top
# level in a background worker, which never loads session_preload_libraries;
# built by the top-level make installcheck and loaded from this directory,
# never installed.

MODULES = toplevel_worker

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
