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
-- whatever session_replication_role is. The two triggers below refuse every
-- INSERT, UPDATE and DELETE that would change a row, which leaves a TRUNCATE
-- that is allowed.
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
-- or on a standby, may rely on any of them. TRUNCATE is refused here too
-- while the table holds records, because a trigger is the only check a
-- replicated TRUNCATE passes. The library also refuses DROP and rewrites of
-- the table, and changes to its triggers.
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

-- Only REPACK (CONCURRENTLY) writes records, from C, without firing
-- triggers. Every other insert is refused: SQL, COPY, and above all logical
-- replication, which would otherwise apply another cluster's records here
-- when a publication FOR ALL TABLES includes this table. Row level, because
-- the apply worker fires no statement-level triggers; ALWAYS, because it
-- runs with session_replication_role = replica.
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
