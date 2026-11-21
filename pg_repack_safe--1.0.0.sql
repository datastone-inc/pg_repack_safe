/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file pg_repack_safe--1.0.0.sql
 * @brief Install script: the unsafe-rewrite record table
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_repack_safe" to load this file. \quit

-- One row per unsafe rewrite: the rewritten relation and the rewriter's
-- full transaction ID. Readers whose snapshot does not see rewrite_xid as
-- committed must not read relid. The 64-bit form keeps a record unambiguous
-- however long it is kept; a 32-bit XID would come to name a newer
-- transaction after about 2^31 transactions. The reader scans the table with
-- heap semantics (SnapshotAny), so the access method is fixed here rather
-- than taken from default_table_access_method.
--
-- The table is deliberately not marked with pg_extension_config_dump(): a
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

-- Readers keep a per-backend copy of the records. The writer invalidates it
-- itself; this trigger does the same for changes made by hand, and fires
-- whatever session_replication_role is.
CREATE FUNCTION pg_repack_safe_records_changed()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

REVOKE ALL ON FUNCTION pg_repack_safe_records_changed() FROM PUBLIC;

CREATE TRIGGER pg_repack_safe_rewrites_changed
    AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON pg_repack_safe_rewrites
    FOR EACH STATEMENT
    EXECUTE FUNCTION pg_repack_safe_records_changed();

ALTER TABLE pg_repack_safe_rewrites
    ENABLE ALWAYS TRIGGER pg_repack_safe_rewrites_changed;

-- Records are never updated or deleted: an older snapshot, on this server
-- or on a standby, may rely on any of them. The library also refuses DROP,
-- TRUNCATE and rewrites of the table, and changes to these triggers.
CREATE FUNCTION pg_repack_safe_records_protected()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

REVOKE ALL ON FUNCTION pg_repack_safe_records_protected() FROM PUBLIC;

CREATE TRIGGER pg_repack_safe_rewrites_protected
    BEFORE UPDATE OR DELETE ON pg_repack_safe_rewrites
    FOR EACH STATEMENT
    EXECUTE FUNCTION pg_repack_safe_records_protected();

ALTER TABLE pg_repack_safe_rewrites
    ENABLE ALWAYS TRIGGER pg_repack_safe_rewrites_protected;
