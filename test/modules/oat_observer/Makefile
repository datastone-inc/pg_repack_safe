# Test-only module for t/015_oat_hook_chain.pl; installed by the top-level
# make installcheck, not by make install.

MODULES = oat_observer

# PostgreSQL's release-build headers have inline functions whose parameters
# are used only under assertions, so unused parameters are not errors.
PG_CPPFLAGS = -Wall -Wextra -Wno-unused-parameter -Werror

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
