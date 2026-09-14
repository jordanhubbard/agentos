/*
 * pd_startup_record.h — Documented startup record for parameterized PDs
 *
 * Several agentOS protection domains are "parameterized": a single ELF image
 * is instantiated more than once (one per slot) and/or needs per-instance peer
 * endpoint capabilities that the generic pd_main(my_ep, ns_ep) signature cannot
 * carry.  Examples covered by this contract:
 *
 *   - swap_slot   (4 instances: slots 0..3; each needs a controller-notify cap)
 *   - app_slot    (per-instance slot id; spawn-server peer endpoint)
 *   - wg_net      (controller-notify cap)
 *   - vibe_swap   (future standalone image: per-slot worker endpoint caps)
 *
 * Historically these wrappers hard-coded slot 0 and a NULL controller cap
 * (see agentos-3ev).  This header replaces that TEMPORARY default with a
 * versioned, packed startup record that the root task populates per instance
 * and the PD reads at boot.
 *
 * Transport
 * ─────────
 * The root task allocates one 4 KiB frame per parameterized PD, writes a
 * populated pd_startup_record_t into it, and maps it read-only into the PD's
 * VSpace at PD_STARTUP_RECORD_VA before starting the PD thread.  This mirrors
 * the existing cc_pd startup-record mechanism (cc_pd.c / main.c).
 *
 * Capability fields are expressed as CNode SLOT NUMBERS within the PD's own
 * CNode (i.e. seL4_CPtr values valid in the PD's capability space), consistent
 * with the PD_CNODE_SLOT_* model in system_desc.h.  A value of
 * PD_STARTUP_CAP_NONE (0) means "no such cap was provisioned".
 *
 * Versioning
 * ──────────
 * pd_startup_record_t carries an explicit magic + version.  Bump
 * PD_STARTUP_RECORD_VERSION and document the change in
 * contracts/<pd>/CHANGELOG when the wire layout changes.  A reader MUST verify
 * magic == PD_STARTUP_RECORD_MAGIC and version == PD_STARTUP_RECORD_VERSION
 * before trusting any other field; on mismatch it falls back to safe defaults
 * (slot 0, no peer caps) exactly as the legacy wrappers did.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef AGENTOS_PD_STARTUP_RECORD_H
#define AGENTOS_PD_STARTUP_RECORD_H

#include <stdint.h>

/*
 * Virtual address at which the root task maps each parameterized PD's startup
 * record frame.  Chosen above the existing fixed PD VAs (IPC buffer 0x10000000,
 * cc_pd device pages 0x10001000..0x10004000).
 *
 * This address is also used for the serial transfer page in DIFFERENT
 * VSpaces. main.c provisions startup records only for swap_slot*, app_slot*,
 * wg_net and vibe_swap; none receives the serial transfer page. Serial
 * clients (serial_pd, log_drain, guest VMMs, cc_pd, net_virt, blk_virt and
 * test_runner) receive no parameterized startup record. If those sets ever
 * intersect, allocate a distinct VA before mapping both frames into a PD.
 *
 * NOTE: this VA is local to the parameterized-PD startup contract.  If a future
 * change wants a project-wide constant in agentos.h, that addition is tracked
 * separately (see agentos-3ev OUT-OF-SCOPE) — agentos.h is owned elsewhere.
 */
#define PD_STARTUP_RECORD_VA   0x10005000UL

/* Magic ("PDSR") and version guarding the record layout. */
#define PD_STARTUP_RECORD_MAGIC    0x50445352u   /* 'P','D','S','R' little-endian */
#define PD_STARTUP_RECORD_VERSION  1u

/* Sentinel CNode slot meaning "no capability provisioned for this field". */
#define PD_STARTUP_CAP_NONE        0u

/* Maximum number of generic peer endpoint caps carried per record. */
#define PD_STARTUP_MAX_PEER_EPS    4u

/*
 * pd_startup_record_t — per-instance startup parameters for a parameterized PD.
 *
 * All fields are little-endian, fixed-width, and packed; the struct is exactly
 * 64 bytes so it fits trivially within one frame with room for future growth.
 *
 * Fields:
 *   magic            PD_STARTUP_RECORD_MAGIC; reader validates before use.
 *   version          PD_STARTUP_RECORD_VERSION; reader validates before use.
 *   slot_id          Per-instance slot index (e.g. swap_slot 0..3, app_slot id).
 *   controller_ntfn  CNode slot of the notification/endpoint cap used to signal
 *                    the controller, or PD_STARTUP_CAP_NONE.  Replaces the
 *                    hard-coded NULL controller cap in the legacy wrappers.
 *   peer_ep_count    Number of valid entries in peer_ep[].
 *   peer_ep          CNode slots of generic peer endpoint caps (meaning is
 *                    PD-specific: e.g. vibe_swap's four swap_slot worker eps, or
 *                    app_slot's spawn-server ep at index 0), each
 *                    PD_STARTUP_CAP_NONE if absent.
 *   reserved         Zeroed; reserved for future fields.  MUST be zero in v1.
 */
typedef struct {
    uint32_t magic;                              /* +0  */
    uint32_t version;                            /* +4  */
    uint32_t slot_id;                            /* +8  */
    uint32_t controller_ntfn;                    /* +12 */
    uint32_t peer_ep_count;                      /* +16 */
    uint32_t peer_ep[PD_STARTUP_MAX_PEER_EPS];   /* +20 .. +35 */
    uint32_t reserved[7];                        /* +36 .. +63 */
} __attribute__((packed)) pd_startup_record_t;

_Static_assert(sizeof(pd_startup_record_t) == 64u,
               "pd_startup_record_t must be exactly 64 bytes");

/*
 * pd_startup_record_valid — true iff the record at *rec carries the expected
 * magic and version.  A reader that gets false MUST fall back to safe defaults
 * (slot 0, no peer caps) rather than trusting uninitialized memory.
 */
static inline int pd_startup_record_valid(const pd_startup_record_t *rec)
{
    return rec != 0 &&
           rec->magic   == PD_STARTUP_RECORD_MAGIC &&
           rec->version == PD_STARTUP_RECORD_VERSION;
}

/*
 * pd_startup_record_slot_id — slot id from the record, or 0 if invalid.
 */
static inline uint32_t pd_startup_record_slot_id(const pd_startup_record_t *rec)
{
    return pd_startup_record_valid(rec) ? rec->slot_id : 0u;
}

/*
 * pd_startup_record_controller_ntfn — controller notify cap slot from the
 * record, or PD_STARTUP_CAP_NONE if invalid/absent.
 */
static inline uint32_t pd_startup_record_controller_ntfn(const pd_startup_record_t *rec)
{
    return pd_startup_record_valid(rec) ? rec->controller_ntfn
                                        : PD_STARTUP_CAP_NONE;
}

/*
 * pd_startup_record_peer_ep — peer endpoint cap slot at index idx, or
 * PD_STARTUP_CAP_NONE if invalid/out-of-range/absent.
 */
static inline uint32_t pd_startup_record_peer_ep(const pd_startup_record_t *rec,
                                                 uint32_t idx)
{
    if (!pd_startup_record_valid(rec) || idx >= PD_STARTUP_MAX_PEER_EPS ||
        idx >= rec->peer_ep_count) {
        return PD_STARTUP_CAP_NONE;
    }
    return rec->peer_ep[idx];
}

/*
 * pd_startup_record_init — zero a record and stamp the current magic/version.
 * The root task calls this, then fills slot_id / controller_ntfn / peer_ep[]
 * before mapping the frame into the PD.
 */
static inline void pd_startup_record_init(pd_startup_record_t *rec)
{
    if (rec == 0) return;
    for (uint32_t i = 0; i < sizeof(*rec) / sizeof(uint32_t); i++) {
        ((volatile uint32_t *)rec)[i] = 0u;
    }
    rec->magic   = PD_STARTUP_RECORD_MAGIC;
    rec->version = PD_STARTUP_RECORD_VERSION;
}

#endif /* AGENTOS_PD_STARTUP_RECORD_H */
