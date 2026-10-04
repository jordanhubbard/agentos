/*
 * agentOS endowment-delta ledger — the delegator's own self-report
 *
 * T4's authority page (platform/authority.h) is a record of what the ROOT
 * TASK granted at boot. Its own header already says what it does not cover:
 *
 *     "It covers the boot-time static set; runtime delegation must be
 *      separately reported by the delegating domain."
 *
 * This is that separate report, and nothing more. When a protection domain
 * creates a child at run time (libs/pd-support/child_spawn.c) and mints
 * capabilities into that child's CSpace, each successful mint is appended
 * here, in the delegating domain's OWN memory. aos_endow_ledger_merge()
 * then folds the whole ledger into an aos_authority_snapshot_t, so the
 * delta is reported through T4's existing shape and formatter rather than
 * through a second, parallel reporting channel invented for T6.
 *
 * ── This is NOT T5's lease table ────────────────────────────────────────
 *
 * libs/pd-support/cap_lease.c records LOANS: temporary authority a domain
 * hands to an already-running peer and intends to withdraw again, keyed by
 * a lease id whose whole purpose is to name something that will later be
 * revoked. An endowment has no such lifetime -- it is a permanent grant
 * made at a child's creation, and the parent has no "take it back"
 * operation for it (deliberately: see child_spawn.h's "Endowment: a MINT,
 * not a loan"). Recording endowments in the lease table would have made
 * every endowment look like an outstanding loan, and would have coupled
 * this report to a revocation primitive that must never run on this path.
 * The two records are separate because the two lifetimes are separate.
 *
 * ── Report, not proof ───────────────────────────────────────────────────
 *
 * Everything platform/authority.h says about itself applies here with
 * full force. seL4 exposes no capability-enumeration syscall
 * (seL4_DebugCapIdentify is CONFIG_DEBUG_BUILD-only and disabled in the
 * shipped release kernel), so this records what the DELEGATING DOMAIN SAYS
 * it granted. It cannot read kernel state and cannot detect a divergence
 * between this record and reality. It does NOT verify the subsetting
 * invariant -- "no domain holds authority its parent did not hold" is
 * enforced by seL4 unconditionally and independently, because a domain
 * cannot mint from a capability it does not possess. A domain that lies
 * here still cannot mint what it does not hold; a domain that is honest
 * here is simply visible. This supplies visibility, not verification.
 *
 * Host-testable: no seL4 headers, no libc I/O. See
 * tests/test_endow_ledger.c.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef AOS_PLATFORM_ENDOW_LEDGER_H
#define AOS_PLATFORM_ENDOW_LEDGER_H

#include <stdint.h>

#include "platform/authority.h"

#define AOS_ENDOW_LEDGER_VERSION   1u

/*
 * Entries one ledger can hold. A delegating domain records one entry per
 * successfully minted capability, so this bounds (children x caps per
 * child) for one delegator. Deliberately small and fixed-size: this
 * structure lives in a PD's own .bss and there is no allocator here.
 */
#define AOS_ENDOW_LEDGER_MAX_ENTRIES 16u

#define AOS_ENDOW_LEDGER_OK            0
#define AOS_ENDOW_LEDGER_ERR_NULL     (-1)
#define AOS_ENDOW_LEDGER_ERR_VERSION  (-2)
#define AOS_ENDOW_LEDGER_ERR_FULL     (-3)
#define AOS_ENDOW_LEDGER_ERR_INVALID  (-4)

/*
 * One minted capability, as reported by the domain that minted it.
 *
 *   child_index -- the delegator's own identifier for the child domain
 *                   this capability went to. Rows in the merged authority
 *                   snapshot are keyed by it, exactly as root's boot rows
 *                   are keyed by pd_index. A delegator creating several
 *                   children numbers them itself; nothing outside the
 *                   delegator assigns or validates this number.
 *   child_name  -- the delegator's name for that child, honoured by
 *                   aos_authority_add() only the first time child_index
 *                   is seen (see its doc comment).
 *   kind        -- an aos_authority_kind_t. Note this is the SAME integer
 *                   space as aos_endow_cap_t.kind (contracts/
 *                   endowment_contract.h), which that layer leaves
 *                   caller-defined and compares only for equality: a
 *                   caller that wants its endowments reported here must
 *                   number its kinds as aos_authority_kind_t values. Any
 *                   value outside the enumeration is bucketed as
 *                   AOS_AUTHORITY_KIND_OTHER by aos_authority_add(),
 *                   never dropped.
 *   rights      -- the rights bitmask requested for the mint, in
 *                   aos_endow_cap_t.rights encoding (bit0 write, bit1
 *                   read, bit2 grant, bit3 grantreply). Recorded for the
 *                   human reading the report; the authority snapshot
 *                   itself counts kinds, not rights.
 *   badge       -- the badge the derivative was minted with.
 */
typedef struct __attribute__((packed)) aos_endow_ledger_entry {
    uint32_t child_index;
    uint8_t  child_name[AOS_AUTHORITY_NAME_LEN];
    uint32_t kind;
    uint32_t rights;
    uint64_t badge;
} aos_endow_ledger_entry_t;

typedef struct __attribute__((packed)) aos_endow_ledger {
    uint32_t version;
    uint32_t count;     /* entries in use, <= AOS_ENDOW_LEDGER_MAX_ENTRIES */
    uint32_t dropped;   /* record calls refused because the table was full */
    uint32_t reserved;
    aos_endow_ledger_entry_t entries[AOS_ENDOW_LEDGER_MAX_ENTRIES];
} aos_endow_ledger_t;

/* Zero the ledger and stamp the version. Safe on NULL (no-op). */
void aos_endow_ledger_init(aos_endow_ledger_t *led);

/*
 * Append one successful mint. Returns AOS_ENDOW_LEDGER_OK, or
 * AOS_ENDOW_LEDGER_ERR_FULL once the table is full (and increments
 * .dropped so the report says so rather than silently under-counting).
 *
 * A NULL ledger is AOS_ENDOW_LEDGER_ERR_NULL and records nothing: a
 * delegator that does not want to self-report passes NULL, and the mint
 * itself is unaffected either way.
 */
int aos_endow_ledger_record(aos_endow_ledger_t *led, uint32_t child_index,
                            const char *child_name, uint32_t kind,
                            uint32_t rights, uint64_t badge);

/*
 * aos_endow_ledger_mark / aos_endow_ledger_rollback — make a spawn's
 * worth of records atomic from the report's point of view.
 *
 * A child that never starts was never endowed, whatever partial mints the
 * kernel accepted before the failure: aos_child_spawn() destroys the
 * child's CNode (and with it every mint already placed in it) and returns
 * an error. Reporting those mints would describe authority held by a
 * domain that does not exist. A caller therefore takes a mark before its
 * endowment loop and rolls back to it on any failure, so the ledger only
 * ever describes children that actually ran.
 *
 * rollback() also restores .dropped, so a failed spawn that overflowed the
 * table does not leave a permanent "entries were lost" claim behind.
 */
uint32_t aos_endow_ledger_mark(const aos_endow_ledger_t *led);
void     aos_endow_ledger_rollback(aos_endow_ledger_t *led, uint32_t mark);

/* Structural check: version and bounds. */
int aos_endow_ledger_validate(const aos_endow_ledger_t *led);

/*
 * Fold every recorded entry into `snap` via aos_authority_add(), creating
 * one row per distinct child_index. `snap` must already have been
 * initialised (aos_authority_init) -- a caller merging a delta onto a copy
 * of root's boot page would init from that page instead, which is why this
 * function never inits for itself.
 *
 * Returns AOS_ENDOW_LEDGER_OK, or an error if either argument is NULL or
 * the ledger fails validation. Saturation and domain-table overflow are
 * reported by the snapshot's own .saturated / .truncated_adds fields, as
 * for any other aos_authority_add() caller.
 */
int aos_endow_ledger_merge(const aos_endow_ledger_t *led,
                           aos_authority_snapshot_t *snap);

#endif /* AOS_PLATFORM_ENDOW_LEDGER_H */
