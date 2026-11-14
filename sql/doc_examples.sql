-- Tests for the SQL examples in README.md and doc/*.md.

-- README.md, Quick Start
CREATE EXTENSION pg_repack_safe;

SELECT extname, extversion FROM pg_extension WHERE extname = 'pg_repack_safe';

DROP EXTENSION pg_repack_safe;
