/*
 * ep_alloc.c — seL4 endpoint pool allocator
 *
 * Manages a static pool of seL4 Endpoint capabilities created during
 * root-task boot.  Endpoints are allocated on demand and keyed by
 * service_id for lookup by the main boot sequence.
 *
 * No libc, no malloc, no dynamic allocation.  All state is in static
 * storage.  Thread-safety is not required: this module is used only
 * during single-threaded boot initialisation.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ep_alloc.h"
#include "ut_alloc.h"

/* ── Module state ─────────────────────────────────────────────────────────── */

/* Maximum number of service → endpoint mappings we can record. */
#define EP_SERVICE_TABLE_SIZE  64u

typedef struct {
    uint16_t   service_id;  /* service key; 0 = empty slot */
    seL4_CPtr  ep_cap;      /* capability to the allocated endpoint  */
} ep_service_entry_t;

static seL4_CPtr            g_root_cnode;
static seL4_Word            g_next_slot;    /* next CNode slot to use */
static seL4_Word            g_slot_limit;   /* exclusive upper bound   */
static ep_service_entry_t   g_svc_table[EP_SERVICE_TABLE_SIZE];
static uint32_t             g_svc_count;

/* ── Initialisation ───────────────────────────────────────────────────────── */

void ep_alloc_init(seL4_CPtr root_cnode,
                   seL4_Word first_slot,
                   uint32_t  pool_size)
{
    g_root_cnode = root_cnode;
    g_next_slot  = first_slot;
    g_slot_limit = first_slot + (seL4_Word)pool_size;
    g_svc_count  = 0u;

    /* Zero the service table */
    for (uint32_t i = 0u; i < EP_SERVICE_TABLE_SIZE; i++) {
        g_svc_table[i].service_id = 0u;
        g_svc_table[i].ep_cap     = seL4_CapNull;
    }
}

/* ── Allocation ───────────────────────────────────────────────────────────── */

seL4_CPtr ep_alloc(void)
{
    if (g_next_slot >= g_slot_limit) {
        /* Pool exhausted */
        return seL4_CapNull;
    }

    seL4_Word slot = g_next_slot;
    seL4_Error err = ut_alloc(seL4_EndpointObject,
                               0u /* size_bits: fixed-size object */,
                               g_root_cnode,
                               slot,
                               64u /* dest_depth */);
    if (err != seL4_NoError) {
        return seL4_CapNull;
    }

    g_next_slot++;
    return slot;
}

/* ── Badge minting ────────────────────────────────────────────────────────── */

/*
 * seL4_CNode_Mint — mint a badged copy of a capability.
 *
 * This is a simplified wrapper: it emits the seL4_CNode_Mint syscall to
 * copy src_cap into dest_cnode[dest_index] with the given badge.
 *
 * On AArch64 and RISC-V seL4 this is invoked via seL4_Call on the CNode
 * cap.  For the root task's early boot phase we use the direct invocation
 * path available through the inline seL4 syscall shim.
 *
 * NOTE: The actual seL4_CNode_Mint implementation is arch-specific and
 * provided by the seL4 kernel syscall interface.  At link time this symbol
 * is resolved from the Microkit libsel4 stub or the simulator.
 */
extern seL4_Error seL4_CNode_Mint(seL4_CPtr  service,
                                   seL4_Word  dest_index,
                                   uint8_t    dest_depth,
                                   seL4_CPtr  src_root,
                                   seL4_Word  src_index,
                                   uint8_t    src_depth,
                                   seL4_Word  rights,
                                   seL4_Word  badge);

seL4_Error ep_mint_badge(seL4_CPtr  src_ep,
                         seL4_Word  badge,
                         seL4_CPtr  dest_cnode,
                         seL4_Word  dest_slot,
                         uint32_t   dest_depth)
{
    return seL4_CNode_Mint(dest_cnode,
                            dest_slot,
                            (uint8_t)dest_depth,
                            g_root_cnode,   /* source root: root task CNode */
                            src_ep,          /* source index: the endpoint cap */
                            64u,             /* source depth */
                            3u /* seL4_AllRights */,
                            badge);
}

/* ── Service-keyed endpoint table ─────────────────────────────────────────── */

seL4_CPtr ep_find_by_service_id(uint16_t service_id)
{
    if (service_id == 0u) {
        return seL4_CapNull;
    }
    for (uint32_t i = 0u; i < g_svc_count; i++) {
        if (g_svc_table[i].service_id == service_id) {
            return g_svc_table[i].ep_cap;
        }
    }
    return seL4_CapNull;
}

seL4_CPtr ep_alloc_for_service(uint16_t service_id)
{
    /* Return existing cap if already registered */
    seL4_CPtr existing = ep_find_by_service_id(service_id);
    if (existing != seL4_CapNull) {
        return existing;
    }

    /* Allocate a new endpoint */
    seL4_CPtr ep = ep_alloc();
    if (ep == seL4_CapNull) {
        return seL4_CapNull;
    }

    /* Register in the service table */
    if (g_svc_count >= EP_SERVICE_TABLE_SIZE) {
        /* Table full — cannot register, but return the cap anyway */
        return ep;
    }

    g_svc_table[g_svc_count].service_id = service_id;
    g_svc_table[g_svc_count].ep_cap     = ep;
    g_svc_count++;

    return ep;
}
