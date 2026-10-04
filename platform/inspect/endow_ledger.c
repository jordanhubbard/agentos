/*
 * Endowment-delta ledger: the delegating domain's self-report of the
 * capabilities it minted into children it created at run time, folded into
 * T4's authority snapshot shape.
 *
 * No seL4. No libc I/O. See platform/include/platform/endow_ledger.h for
 * what this is and, importantly, what it is not (a report, not a proof;
 * and not T5's lease table).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <platform/endow_ledger.h>

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(aos_endow_ledger_t) <= 4096,
               "endowment ledger fits one page");

static void copy_name(uint8_t dst[AOS_AUTHORITY_NAME_LEN], const char *src)
{
    uint32_t i;

    memset(dst, 0, AOS_AUTHORITY_NAME_LEN);
    if (src == NULL) {
        return;
    }
    for (i = 0u; i < AOS_AUTHORITY_NAME_LEN - 1u && src[i] != '\0'; i++) {
        dst[i] = (uint8_t)src[i];
    }
}

void aos_endow_ledger_init(aos_endow_ledger_t *led)
{
    if (led == NULL) {
        return;
    }
    memset(led, 0, sizeof(*led));
    led->version = AOS_ENDOW_LEDGER_VERSION;
}

int aos_endow_ledger_record(aos_endow_ledger_t *led, uint32_t child_index,
                            const char *child_name, uint32_t kind,
                            uint32_t rights, uint64_t badge)
{
    aos_endow_ledger_entry_t *e;

    if (led == NULL) {
        return AOS_ENDOW_LEDGER_ERR_NULL;
    }
    if (led->version != AOS_ENDOW_LEDGER_VERSION) {
        return AOS_ENDOW_LEDGER_ERR_VERSION;
    }
    if (led->count >= AOS_ENDOW_LEDGER_MAX_ENTRIES) {
        /* Say so rather than silently under-report: a reader that sees
         * dropped != 0 knows the delta is incomplete. */
        led->dropped++;
        return AOS_ENDOW_LEDGER_ERR_FULL;
    }

    e = &led->entries[led->count];
    memset(e, 0, sizeof(*e));
    e->child_index = child_index;
    copy_name(e->child_name, child_name);
    e->kind   = kind;
    e->rights = rights;
    e->badge  = badge;
    led->count++;
    return AOS_ENDOW_LEDGER_OK;
}

uint32_t aos_endow_ledger_mark(const aos_endow_ledger_t *led)
{
    if (led == NULL) {
        return 0u;
    }
    return led->count;
}

void aos_endow_ledger_rollback(aos_endow_ledger_t *led, uint32_t mark)
{
    if (led == NULL || mark > led->count) {
        return;
    }
    /* Zero the abandoned entries rather than just lowering the count: a
     * ledger page that a reader may map should not retain the name of a
     * child that never existed in its tail. */
    memset(&led->entries[mark], 0,
           (size_t)(led->count - mark) * sizeof(led->entries[0]));
    led->count = mark;
    /* A spawn that overflowed the table and then failed must not leave a
     * permanent "entries were lost" claim behind -- nothing was granted. */
    led->dropped = 0u;
}

int aos_endow_ledger_validate(const aos_endow_ledger_t *led)
{
    if (led == NULL) {
        return AOS_ENDOW_LEDGER_ERR_NULL;
    }
    if (led->version != AOS_ENDOW_LEDGER_VERSION) {
        return AOS_ENDOW_LEDGER_ERR_VERSION;
    }
    if (led->count > AOS_ENDOW_LEDGER_MAX_ENTRIES) {
        return AOS_ENDOW_LEDGER_ERR_INVALID;
    }
    return AOS_ENDOW_LEDGER_OK;
}

int aos_endow_ledger_merge(const aos_endow_ledger_t *led,
                           aos_authority_snapshot_t *snap)
{
    uint32_t i;
    int rc;

    if (snap == NULL) {
        return AOS_ENDOW_LEDGER_ERR_NULL;
    }
    rc = aos_endow_ledger_validate(led);
    if (rc != AOS_ENDOW_LEDGER_OK) {
        return rc;
    }

    for (i = 0u; i < led->count; i++) {
        const aos_endow_ledger_entry_t *e = &led->entries[i];
        /*
         * Pass the name on EVERY call, not only the first: aos_authority_add
         * honours it when the row is created and ignores it afterwards, and
         * its doc comment asks callers to do exactly this because a caller
         * cannot generally know which entry will create the row.
         */
        (void)aos_authority_add(snap, e->child_index,
                                (const char *)e->child_name, e->kind);
    }
    return AOS_ENDOW_LEDGER_OK;
}
