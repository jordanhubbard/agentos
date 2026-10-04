/*
 * Capability-lease bookkeeping table: implementation.
 *
 * See kernel/agentos-root-task/include/contracts/lease_contract.h for the
 * semantics this must hold. No seL4 calls, no interpretation of `rights`.
 *
 * lease_id is the slot's index into the table. That keeps lookup O(1) and
 * bounds-checking trivial; ids are only guaranteed distinct among the
 * leases currently open. Once a lease is closed and its slot reused, the
 * freed id may be handed out again to a new, unrelated lease.
 *
 * A closed slot is left in state AOS_LEASE_REVOKED, not reset straight
 * back to AOS_LEASE_FREE, so a report can still tell "never used" apart
 * from "used and withdrawn" right up until the slot is reclaimed by the
 * next aos_lease_open(). aos_lease_open() treats both FREE and REVOKED
 * slots as available to claim — "frees the slot for reuse" means exactly
 * that a REVOKED slot is eligible, not that it silently reverts to FREE.
 */
#include "contracts/lease_contract.h"

void aos_lease_table_init(aos_lease_table_t *table)
{
    if (table == NULL) {
        return;
    }
    for (uint32_t i = 0; i < AOS_LEASE_MAX_ACTIVE; i++) {
        table->slots[i].lease_id = i;
        table->slots[i].borrower_pd = 0;
        table->slots[i].rights = 0;
        table->slots[i].state = AOS_LEASE_FREE;
    }
}

int aos_lease_open(aos_lease_table_t *table, uint32_t borrower_pd,
                    uint32_t rights, uint32_t *out_id)
{
    if (table == NULL || out_id == NULL) {
        return -1;
    }

    for (uint32_t i = 0; i < AOS_LEASE_MAX_ACTIVE; i++) {
        if (table->slots[i].state != AOS_LEASE_ACTIVE) {
            table->slots[i].lease_id = i;
            table->slots[i].borrower_pd = borrower_pd;
            table->slots[i].rights = rights;
            table->slots[i].state = AOS_LEASE_ACTIVE;
            *out_id = i;
            return 0;
        }
    }

    /* No free slot: AOS_LEASE_MAX_ACTIVE leases already open. Fail rather
     * than overwrite a live slot. */
    return -1;
}

int aos_lease_close(aos_lease_table_t *table, uint32_t lease_id)
{
    if (table == NULL || lease_id >= AOS_LEASE_MAX_ACTIVE) {
        return -1;
    }

    aos_lease_t *slot = &table->slots[lease_id];
    if (slot->state != AOS_LEASE_ACTIVE) {
        /* Unknown, out of range (handled above), or already closed:
         * fail rather than silently succeeding. */
        return -1;
    }

    slot->state = AOS_LEASE_REVOKED;
    return 0;
}

const aos_lease_t *aos_lease_get(const aos_lease_table_t *table,
                                  uint32_t lease_id)
{
    if (table == NULL || lease_id >= AOS_LEASE_MAX_ACTIVE) {
        return NULL;
    }
    return &table->slots[lease_id];
}
