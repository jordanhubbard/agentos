/*
 * Host test: capability-lease bookkeeping table.
 *
 * This pins the semantics a lender depends on to close leases
 * deterministically and to report state truthfully:
 *   - a fresh table has no active leases;
 *   - aos_lease_open() returns a distinct id and marks the slot ACTIVE;
 *   - opening beyond AOS_LEASE_MAX_ACTIVE fails rather than overwriting
 *     a live slot;
 *   - aos_lease_close() marks REVOKED and frees the slot for reuse;
 *   - closing an unknown id, or an already-closed id, fails rather than
 *     silently succeeding — a lender must never be told "closed" for a
 *     lease that is still live;
 *   - aos_lease_get() bounds-checks its id rather than indexing blind.
 *
 * No seL4 calls here. This is bookkeeping only.
 */
#include <assert.h>
#include <stdio.h>
#include "contracts/lease_contract.h"

int main(void)
{
    aos_lease_table_t table;
    aos_lease_table_init(&table);

    /* A fresh table has no active leases. */
    for (uint32_t i = 0; i < AOS_LEASE_MAX_ACTIVE; i++) {
        const aos_lease_t *l = aos_lease_get(&table, i);
        assert(l != NULL);
        assert(l->state == AOS_LEASE_FREE);
    }

    /* Opening returns a distinct id and marks ACTIVE. */
    uint32_t id0, id1;
    assert(aos_lease_open(&table, 100, 0x1, &id0) == 0);
    assert(aos_lease_open(&table, 200, 0x2, &id1) == 0);
    assert(id0 != id1);

    const aos_lease_t *l0 = aos_lease_get(&table, id0);
    assert(l0 != NULL);
    assert(l0->lease_id == id0);
    assert(l0->borrower_pd == 100);
    assert(l0->rights == 0x1);
    assert(l0->state == AOS_LEASE_ACTIVE);

    const aos_lease_t *l1 = aos_lease_get(&table, id1);
    assert(l1 != NULL);
    assert(l1->borrower_pd == 200);
    assert(l1->rights == 0x2);
    assert(l1->state == AOS_LEASE_ACTIVE);

    /* Opening more than AOS_LEASE_MAX_ACTIVE fails rather than
     * overwriting an existing lease. Two slots are already taken above. */
    uint32_t extra_ids[AOS_LEASE_MAX_ACTIVE];
    uint32_t opened = 0;
    for (uint32_t i = 0; i < AOS_LEASE_MAX_ACTIVE; i++) {
        uint32_t out_id;
        int rc = aos_lease_open(&table, 300 + i, 0x4, &out_id);
        if (rc != 0) break;
        extra_ids[opened++] = out_id;
    }
    /* Exactly AOS_LEASE_MAX_ACTIVE - 2 more should have succeeded, since
     * id0 and id1 already occupy two slots. */
    assert(opened == AOS_LEASE_MAX_ACTIVE - 2);
    {
        uint32_t out_id;
        assert(aos_lease_open(&table, 999, 0x8, &out_id) != 0);
    }

    /* The two original leases are untouched by the failed opens. */
    l0 = aos_lease_get(&table, id0);
    assert(l0->state == AOS_LEASE_ACTIVE);
    assert(l0->borrower_pd == 100);
    l1 = aos_lease_get(&table, id1);
    assert(l1->state == AOS_LEASE_ACTIVE);
    assert(l1->borrower_pd == 200);

    /* Closing marks REVOKED and frees the slot for reuse. */
    assert(aos_lease_close(&table, id0) == 0);
    l0 = aos_lease_get(&table, id0);
    assert(l0 != NULL);
    assert(l0->state == AOS_LEASE_REVOKED);

    /* Closing an unknown id fails. */
    assert(aos_lease_close(&table, 0xdeadbeef) != 0);

    /* Closing an already-closed id fails rather than silently succeeding. */
    assert(aos_lease_close(&table, id0) != 0);

    /* The freed slot can be reused by a new open. */
    {
        uint32_t new_id;
        int rc = aos_lease_open(&table, 555, 0x10, &new_id);
        assert(rc == 0);
        const aos_lease_t *l = aos_lease_get(&table, new_id);
        assert(l != NULL);
        assert(l->state == AOS_LEASE_ACTIVE);
        assert(l->borrower_pd == 555);
        assert(l->rights == 0x10);
    }

    /* aos_lease_get() bounds-checks its id rather than indexing blind. */
    assert(aos_lease_get(&table, AOS_LEASE_MAX_ACTIVE) == NULL);
    assert(aos_lease_get(&table, AOS_LEASE_MAX_ACTIVE + 1000) == NULL);
    assert(aos_lease_get(&table, 0xffffffffu) == NULL);

    /*
     * ABA protection: a stale id from a lease whose slot was reclaimed
     * must not alias the new, unrelated lease now occupying that slot.
     * Use a fresh table so the slot being exercised is unambiguous.
     */
    {
        aos_lease_table_t t2;
        aos_lease_table_init(&t2);

        uint32_t a_id;
        assert(aos_lease_open(&t2, 10, 0x1, &a_id) == 0);
        assert(aos_lease_close(&t2, a_id) == 0);

        /* B reclaims A's slot (the only slot touched so far). */
        uint32_t b_id;
        assert(aos_lease_open(&t2, 20, 0x2, &b_id) == 0);
        assert(b_id != a_id);

        /* Closing with A's stale id must fail outright, and must not
         * revoke B's still-active lease. */
        assert(aos_lease_close(&t2, a_id) != 0);
        {
            const aos_lease_t *b = aos_lease_get(&t2, b_id);
            assert(b != NULL);
            assert(b->state == AOS_LEASE_ACTIVE);
            assert(b->borrower_pd == 20);
        }

        /* aos_lease_get() on A's stale id must not alias B either. */
        assert(aos_lease_get(&t2, a_id) == NULL);

        /* Happy path across many open/close cycles reusing the same
         * slot: each new id differs from its predecessor's, and the
         * predecessor's id goes stale the moment it is superseded. */
        uint32_t prev_id = b_id;
        for (int cycle = 0; cycle < 50; cycle++) {
            assert(aos_lease_close(&t2, prev_id) == 0);

            uint32_t next_id;
            assert(aos_lease_open(&t2, 1000u + (uint32_t)cycle, 0x3,
                                   &next_id) == 0);
            assert(next_id != prev_id);

            const aos_lease_t *l = aos_lease_get(&t2, next_id);
            assert(l != NULL);
            assert(l->state == AOS_LEASE_ACTIVE);
            assert(l->borrower_pd == 1000u + (uint32_t)cycle);

            /* The superseded id is now stale. */
            assert(aos_lease_get(&t2, prev_id) == NULL);
            assert(aos_lease_close(&t2, prev_id) != 0);

            prev_id = next_id;
        }
    }

    printf("PASS: test_cap_lease\n");
    return 0;
}
