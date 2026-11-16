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
-- transaction after about 2^31 transactions. The reader scans the table with heap
-- semantics (SnapshotAny), so the access method is fixed here rather than
-- taken from default_table_access_method.
CREATE TABLE pg_repack_safe_rewrites
(
    relid oid NOT NULL,
    rewrite_xid xid8 NOT NULL
) USING heap;

REVOKE ALL ON TABLE pg_repack_safe_rewrites FROM PUBLIC;
