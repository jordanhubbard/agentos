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

/*
 * Only endpoint and notification capabilities carry a badge. seL4 silently
 * ignores the badge argument to seL4_CNode_Mint for every other type -- an
 * ARM page capability minted with 0xC417 carries no badge at all -- so
 * recording the requested value for those kinds would put a field in this
 * record that does not describe anything the child holds. A record whose
 * entire purpose is accurate self-reporting does not get to contain a
 * number that is simply false, however harmless it is downstream (the
 * authority snapshot counts kinds, not badges).
 *
 * AOS_AUTHORITY_KIND_OTHER is treated as badge-carrying: it is the bucket
 * for kinds this enumeration does not name, so the honest thing is to
 * preserve what the caller passed rather than discard it on a guess.
 */
static int kind_carries_badge(uint32_t kind)
{
    return kind == (uint32_t)AOS_AUTHORITY_KIND_ENDPOINT
        || kind == (uint32_t)AOS_AUTHORITY_KIND_NOTIFICATION
        || kind >= AOS_AUTHORITY_KIND_COUNT
        || kind == (uint32_t)AOS_AUTHORITY_KIND_OTHER;
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
    e->badge  = kind_carries_badge(kind) ? badge : 0u;
    led->count++;
    return AOS_ENDOW_LEDGER_OK;
}

aos_endow_ledger_mark_t aos_endow_ledger_mark(const aos_endow_ledger_t *led)
{
    aos_endow_ledger_mark_t mark = {0u, 0u};

    if (led == NULL) {
        return mark;
    }
    mark.count   = led->count;
    mark.dropped = led->dropped;
    return mark;
}

void aos_endow_ledger_rollback(aos_endow_ledger_t *led,
                               aos_endow_ledger_mark_t mark)
{
    if (led == NULL || mark.count > led->count || mark.dropped > led->dropped) {
        return;
    }
    /* Zero the abandoned entries rather than just lowering the count: a
     * ledger page that a reader may map should not retain the name of a
     * child that never existed in its tail. */
    memset(&led->entries[mark.count], 0,
           (size_t)(led->count - mark.count) * sizeof(led->entries[0]));
    led->count = mark.count;
    /* RESTORE, not clear. Drop only the overflows this spawn itself caused;
     * any that were already outstanding belong to a child that is running
     * right now with a capability this record does not list, and erasing
     * that flag would make an incomplete report claim to be a complete one.
     * See the header. */
    led->dropped = mark.dropped;
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
