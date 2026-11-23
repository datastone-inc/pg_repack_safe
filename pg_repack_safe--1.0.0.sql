/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file pg_repack_safe--1.0.0.sql
 * @brief Install script: the unsafe-rewrite record table, the triggers that
 *        guard it, and the SQL functions
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_repack_safe" to load this file. \quit

-- One row per unsafe rewrite: the rewritten relation and the rewriter's
-- full transaction ID. Readers whose snapshot does not see rewrite_xid as
-- committed must not read relid. The reader scans the table with heap
-- semantics (SnapshotAny), so the access method is fixed here rather than
-- taken from default_table_access_method.
--
-- The table is not marked with pg_extension_config_dump(): a
-- record's relation OID and transaction ID mean something only in the
-- database and cluster that wrote it, so pg_dump must not carry the rows
-- into another one, where they would refuse reads of unrelated tables. No
-- snapshot survives a dump and restore, so no record is needed there.
CREATE TABLE pg_repack_safe_rewrites
(
    relid oid NOT NULL,
    rewrite_xid xid8 NOT NULL
) USING heap;

REVOKE ALL ON TABLE pg_repack_safe_rewrites FROM PUBLIC;

-- No SQL statement changes a record. The library alone writes and deletes
-- records, from C, without firing triggers, and invalidates the per-backend
-- copy of the records that readers keep. The two triggers below refuse
-- every other change: the first every INSERT, the second every UPDATE and
-- DELETE, and a TRUNCATE while the table holds records. The library also
-- refuses DROP and rewrites of the table, and changes to its triggers.

-- Only REPACK (CONCURRENTLY) writes records. Every other insert is refused:
-- SQL, COPY, and logical replication, which would otherwise apply
-- another cluster's records here when a publication FOR ALL TABLES includes
-- this table. Row level, because the apply worker fires no statement-level
-- triggers; ALWAYS, because it runs with session_replication_role = replica.
CREATE FUNCTION pg_repack_safe_records_writer_only()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

REVOKE ALL ON FUNCTION pg_repack_safe_records_writer_only() FROM PUBLIC;

CREATE TRIGGER pg_repack_safe_rewrites_writer_only
    BEFORE INSERT ON pg_repack_safe_rewrites
    FOR EACH ROW
    EXECUTE FUNCTION pg_repack_safe_records_writer_only();

ALTER TABLE pg_repack_safe_rewrites
    ENABLE ALWAYS TRIGGER pg_repack_safe_rewrites_writer_only;

-- Records are never updated, and deleted only by the library (each REPACK
-- (CONCURRENTLY), and pg_repack_safe_prune_records below): an older
-- snapshot, on this server or on a standby, may rely on any of them, and
-- the library deletes only in a way that keeps the record until no such
-- snapshot is left. TRUNCATE is refused while the table holds records,
-- because a trigger is the only check a replicated TRUNCATE passes.
CREATE FUNCTION pg_repack_safe_records_protected()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

REVOKE ALL ON FUNCTION pg_repack_safe_records_protected() FROM PUBLIC;

CREATE TRIGGER pg_repack_safe_rewrites_protected
    BEFORE UPDATE OR DELETE OR TRUNCATE ON pg_repack_safe_rewrites
    FOR EACH STATEMENT
    EXECUTE FUNCTION pg_repack_safe_records_protected();

ALTER TABLE pg_repack_safe_rewrites
    ENABLE ALWAYS TRIGGER pg_repack_safe_rewrites_protected;

-- Each REPACK (CONCURRENTLY) deletes the records older than its own. This
-- does the same on request, for a database that has stopped repacking and
-- before removing the extension.
CREATE FUNCTION pg_repack_safe_prune_records()
RETURNS bigint
AS 'MODULE_PATHNAME'
LANGUAGE C VOLATILE;

REVOKE ALL ON FUNCTION pg_repack_safe_prune_records() FROM PUBLIC;

-- Whether the guard is in force on this server: true if it loaded the
-- library through shared_preload_libraries. Without that, the extension
-- still looks installed but guards nothing: REPACK (CONCURRENTLY) writes no
-- record and no read is checked. The function then returns false with a
-- WARNING that says so. The extension's comment, which \dx shows, points
-- here. Calling the function loads the library in the calling backend if it
-- was not loaded, and fails if the library is not installed at all. It
-- reveals no more than SHOW shared_preload_libraries, so any role may call
-- it.
CREATE FUNCTION pg_repack_safe_status()
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C VOLATILE;
