/*
 * Copyright (c) 2026 dataStone Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file oat_observer.c
 * @brief Test-only module: a second object_access_hook, for chaining tests
 *
 * Reports every OAT_DROP as a NOTICE, then calls the previous hook.
 * t/015_oat_hook_chain.pl loads it next to pg_repack_safe in both orders and
 * checks that both modules' hooks run. Not part of the extension; installed
 * only by make installcheck.
 */

#include "postgres.h"

#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

void        _PG_init(void);

static object_access_hook_type prevObjectAccessHook = NULL;

/**
 * @brief object_access_hook: report a drop, then chain
 * @param access Kind of access being reported
 * @param classId Catalog the object belongs to
 * @param objectId OID of the object
 * @param subId Column number, or 0 for the whole object
 * @param arg Access-type-specific argument
 */
static void
observerObjectAccess(ObjectAccessType access, Oid classId, Oid objectId,
                     int subId, void *arg)
{
    if (access == OAT_DROP)
    {
        ObjectAddress address;

        ObjectAddressSubSet(address, classId, objectId, subId);
        ereport(NOTICE,
                (errmsg("oat_observer: drop of %s",
                        getObjectDescription(&address, true))));
    }

    if (prevObjectAccessHook != NULL)
        prevObjectAccessHook(access, classId, objectId, subId, arg);
}

/**
 * @brief Module load: install the hook
 */
void
_PG_init(void)
{
    prevObjectAccessHook = object_access_hook;
    object_access_hook = observerObjectAccess;
}
