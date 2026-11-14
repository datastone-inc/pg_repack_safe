# Makefile for the pg_repack_safe PostgreSQL extension

EXTENSION = pg_repack_safe
DATA = pg_repack_safe--1.0.0.sql

# doc_examples: validates SQL examples from README.md and doc/*.md
REGRESS = doc_examples

# Build configuration
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Compiler flags
PG_CPPFLAGS = -Wall -Wextra -Werror
