/*
 * Capability-lease bookkeeping contract.
 *
 * This is the lender-side ledger of which leases are currently open. It
 * exists so a lender can close a loan deterministically (by id) and so
 * lease state can be reported. It is bookkeeping only:
 *
 *   - it does NOT call seL4 — minting, transferring, and revoking the
 *     underlying capability is a separate layer (aos_cap_lend /
 *     aos_cap_lend_revoke) built on top of this table, not inside it;
 *   - it does NOT interpret `rights` — the value is opaque here and is
 *     stored only so it can be reported back to a caller or an operator;
 *   - it does NOT know what a capability is — `borrower_pd` and `rights`
 *     are plain integers, not seL4 types.
 *
 * Honesty note (see docs/superpowers/plans/2026-10-04-t5-capability-lending.md,
 * "The ledger cannot verify a lease"): this table only records what a
 * lender tells it. A lease that is never opened here is invisible to any
 * report built from this table, even though the kernel still enforces the
 * underlying authority relation independent of whether it was recorded.
 *
 * Slot semantics:
 *   - A fresh table has every slot AOS_LEASE_FREE.
 *   - aos_lease_open() claims a free slot, marks it AOS_LEASE_ACTIVE, and
 *     returns its id. Opening beyond AOS_LEASE_MAX_ACTIVE concurrently
 *     open leases fails rather than overwriting a live slot.
 *   - aos_lease_close() marks an ACTIVE slot AOS_LEASE_REVOKED and frees
 *     it for reuse. Closing an id that is unknown, out of range, or
 *     already REVOKED fails rather than silently succeeding — a lender
 *     that is told "closed" for a lease that never closed would believe
 *     authority was withdrawn when it was not.
 *   - aos_lease_get() bounds-checks its id and returns NULL rather than
 *     indexing blind for an out-of-range id.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define AOS_LEASE_VERSION 1u

/* Maximum number of leases a single table may track as open at once. */
#define AOS_LEASE_MAX_ACTIVE 8u

typedef enum {
    AOS_LEASE_FREE    = 0u, /* slot unused / available for a new lease */
    AOS_LEASE_ACTIVE  = 1u, /* lease open, borrower currently holds it */
    AOS_LEASE_REVOKED = 2u, /* lease was open, now closed */
} aos_lease_state_t;

typedef struct {
    uint32_t lease_id;
    uint32_t borrower_pd;
    uint32_t rights;
    uint32_t state; /* aos_lease_state_t */
} aos_lease_t;

typedef struct {
    aos_lease_t slots[AOS_LEASE_MAX_ACTIVE];
} aos_lease_table_t;

/* Initialize (or reset) a table: every slot becomes AOS_LEASE_FREE. */
void aos_lease_table_init(aos_lease_table_t *table);

/*
 * Open a new lease recording `borrower_pd` and `rights` (both opaque to
 * this layer). On success writes the new lease's id to *out_id, marks the
 * slot AOS_LEASE_ACTIVE, and returns 0. Returns nonzero and leaves the
 * table unchanged if no free slot exists (AOS_LEASE_MAX_ACTIVE leases are
 * already open) or if any argument is invalid.
 */
int aos_lease_open(aos_lease_table_t *table, uint32_t borrower_pd,
                    uint32_t rights, uint32_t *out_id);

/*
 * Close an open lease by id: marks it AOS_LEASE_REVOKED and frees the
 * slot for reuse by a future aos_lease_open(). Returns 0 on success.
 * Returns nonzero, without changing any state, if lease_id is out of
 * range, unknown, or already AOS_LEASE_REVOKED / AOS_LEASE_FREE.
 */
int aos_lease_close(aos_lease_table_t *table, uint32_t lease_id);

/*
 * Look up a lease by id. Bounds-checks lease_id; returns NULL for any id
 * that is not a currently valid slot index rather than indexing blind.
 * The returned pointer is valid for FREE, ACTIVE, and REVOKED slots alike
 * so state can be reported either way.
 */
const aos_lease_t *aos_lease_get(const aos_lease_table_t *table,
                                  uint32_t lease_id);
