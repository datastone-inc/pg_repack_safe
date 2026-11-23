/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file status.sql
 * @brief repack_safe.status() on a server that preloads the library, the
 *        extension's fixed schema, and the library's version
 *
 * The extension can exist in a database whose server does not preload the
 * library; it then looks installed and guards nothing. The status function
 * returns true only on a server that preloads it, and false with a WARNING
 * otherwise; the extension's comment, which \dx shows, points to it. The
 * not-preloaded case is in t/009_not_preloaded.pl.
 */

-- The extension's schema is fixed: it cannot be created in another one.
CREATE EXTENSION pg_repack_safe SCHEMA public;

CREATE EXTENSION pg_repack_safe;

-- \dx shows the comment, which names the check.
SELECT obj_description(oid, 'pg_extension') AS comment
FROM pg_extension WHERE extname = 'pg_repack_safe';

-- Its objects are in the schema repack_safe, which CREATE EXTENSION
-- created, and it is version 1.0.
SELECT extnamespace::regnamespace AS schema, extrelocatable AS relocatable,
       extversion AS version
FROM pg_extension WHERE extname = 'pg_repack_safe';

-- pg_get_loaded_modules() reports the library's name and that it has a
-- version, on any server that loaded it, with or without the extension.
SELECT module_name, version ~ '^[0-9]+\.[0-9]+\.[0-9]+$' AS has_version
FROM pg_get_loaded_modules() WHERE module_name = 'pg_repack_safe';

-- This instance preloads the library (test/temp_instance.conf): true, and
-- no warning.
SELECT repack_safe.status();

-- Any role may call it.
CREATE ROLE status_user;
SET ROLE status_user;
SELECT repack_safe.status();
RESET ROLE;
DROP ROLE status_user;

-- With the package uninstalled, the call fails with "could not access
-- file" (README, "Removing the extension", step 5). The installed library
-- cannot be removed here, since the other suites use it, so this pins the
-- error PostgreSQL raises for the same function in a file that does not
-- exist. It raises it as soon as the function is declared, since CREATE
-- FUNCTION loads a C function's file.
CREATE FUNCTION status_missing_library() RETURNS boolean
AS '$libdir/pg_repack_safe_missing', 'pg_repack_safe_status'
LANGUAGE C;

DROP EXTENSION pg_repack_safe;

-- After DROP EXTENSION the function is gone with it (README, "Removing the
-- extension", step 5).
SELECT repack_safe.status();

-- DROP EXTENSION leaves the schema, which is not a member of the extension.
SELECT count(*) AS schemas FROM pg_namespace WHERE nspname = 'repack_safe';
DROP SCHEMA repack_safe;
