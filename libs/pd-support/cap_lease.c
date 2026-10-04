/*
 * Capability-lease bookkeeping table: implementation.
 *
 * See kernel/agentos-root-task/include/contracts/lease_contract.h for the
 * semantics this must hold. No seL4 calls, no interpretation of `rights`.
 *
 * lease_id packs a slot index (low AOS_LEASE_SLOT_BITS bits) and a
 * per-slot generation counter (remaining high bits). The generation is
 * bumped every time aos_lease_open() reclaims a slot that was previously
 * used (state AOS_LEASE_REVOKED), so a stale id held past a lease's
 * closure cannot alias whatever new, unrelated lease later lands in the
 * same slot: aos_lease_close() and aos_lease_get() both require the
 * id's generation to match the slot's current generation and fail
 * otherwise. This is the ABA fix — see the header for the full
 * rationale and the residual risk if a slot's generation counter wraps.
 *
 * A closed slot is left in state AOS_LEASE_REVOKED, not reset straight
 * back to AOS_LEASE_FREE, so a report can still tell "never used" apart
 * from "used and withdrawn" right up until the slot is reclaimed by the
 * next aos_lease_open(). aos_lease_open() treats both FREE and REVOKED
 * slots as available to claim — "frees the slot for reuse" means exactly
 * that a REVOKED slot is eligible, not that it silently reverts to FREE.
 */
#include "contracts/lease_contract.h"

static uint32_t lease_make_id(uint32_t slot_idx, uint32_t generation)
{
    return (slot_idx & AOS_LEASE_SLOT_MASK) |
           ((generation & AOS_LEASE_GEN_MASK) << AOS_LEASE_SLOT_BITS);
}

static uint32_t lease_id_slot(uint32_t lease_id)
{
    return lease_id & AOS_LEASE_SLOT_MASK;
}

static uint32_t lease_id_gen(uint32_t lease_id)
{
    return (lease_id >> AOS_LEASE_SLOT_BITS) & AOS_LEASE_GEN_MASK;
}

void aos_lease_table_init(aos_lease_table_t *table)
{
    if (table == NULL) {
        return;
    }
    for (uint32_t i = 0; i < AOS_LEASE_MAX_ACTIVE; i++) {
        table->slots[i].lease_id = lease_make_id(i, 0);
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
        aos_lease_t *slot = &table->slots[i];
        if (slot->state != AOS_LEASE_ACTIVE) {
            uint32_t generation = lease_id_gen(slot->lease_id);
            if (slot->state == AOS_LEASE_REVOKED) {
                /* Reclaiming a previously-used slot: bump the generation
                 * so this lease's id differs from its predecessor's,
                 * even though it is the same slot index. */
                generation = (generation + 1u) & AOS_LEASE_GEN_MASK;
            }
            uint32_t new_id = lease_make_id(i, generation);
            slot->lease_id = new_id;
            slot->borrower_pd = borrower_pd;
            slot->rights = rights;
            slot->state = AOS_LEASE_ACTIVE;
            *out_id = new_id;
            return 0;
        }
    }

    /* No free slot: AOS_LEASE_MAX_ACTIVE leases already open. Fail rather
     * than overwrite a live slot. */
    return -1;
}

int aos_lease_close(aos_lease_table_t *table, uint32_t lease_id)
{
    if (table == NULL) {
        return -1;
    }

    uint32_t idx = lease_id_slot(lease_id);
    if (idx >= AOS_LEASE_MAX_ACTIVE) {
        return -1;
    }

    aos_lease_t *slot = &table->slots[idx];
    if (slot->state != AOS_LEASE_ACTIVE) {
        /* Unknown, out of range (handled above), or already closed:
         * fail rather than silently succeeding. */
        return -1;
    }
    if (slot->lease_id != lease_id) {
        /* Slot index matches but the generation does not: this id
         * belonged to a lease that has already been superseded by a
         * newer one in the same slot. Fail rather than revoking the
         * current occupant's lease out from under it. */
        return -1;
    }

    slot->state = AOS_LEASE_REVOKED;
    return 0;
}

const aos_lease_t *aos_lease_get(const aos_lease_table_t *table,
                                  uint32_t lease_id)
{
    if (table == NULL) {
        return NULL;
    }

    uint32_t idx = lease_id_slot(lease_id);
    if (idx >= AOS_LEASE_MAX_ACTIVE) {
        return NULL;
    }

    const aos_lease_t *slot = &table->slots[idx];
    if (slot->lease_id != lease_id) {
        /* Stale generation: do not alias whatever newer lease currently
         * occupies this slot. */
        return NULL;
    }
    return slot;
}
