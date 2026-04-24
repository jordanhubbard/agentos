/*
 * ep_alloc.h — seL4 endpoint pool allocator for root-task boot
 *
 * Manages a fixed pool of seL4 Endpoint objects allocated from untyped
 * memory during boot.  Each service protection domain that needs to
 * receive IPC is assigned one endpoint; callers receive minted (badged)
 * copies of the endpoint capability.
 *
 * Usage pattern (root task boot):
 *
 *   ep_alloc_init(root_cnode, first_slot, pool_size);
 *
 *   seL4_CPtr ep = ep_alloc();          // create raw endpoint
 *   seL4_CPtr minted = ep_mint_badge(ep, badge, dest_cnode,
 *                                     dest_slot, dest_depth);
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "sel4_boot.h"
#include <stdint.h>

/*
 * ep_alloc_init — initialise the endpoint pool.
 *
 * Parameters:
 *   root_cnode   root CNode capability (seL4_CapInitThreadCNode)
 *   first_slot   first free CNode slot to use for endpoint caps
 *   pool_size    number of endpoint caps to pre-allocate
 *
 * The allocator reserves [first_slot, first_slot + pool_size) slots in
 * the root CNode.  Each slot will hold one seL4 Endpoint object capability
 * created via ut_alloc on first use.
 */
void ep_alloc_init(seL4_CPtr root_cnode,
                   seL4_Word first_slot,
                   uint32_t  pool_size);

/*
 * ep_alloc — allocate one endpoint from the pool.
 *
 * Retypes untyped memory into a seL4_EndpointObject and places the
 * resulting capability into the next free slot in the pre-allocated range.
 *
 * Returns the seL4_CPtr of the newly-created endpoint on success, or
 * seL4_CapNull if the pool is exhausted or allocation fails.
 */
seL4_CPtr ep_alloc(void);

/*
 * ep_mint_badge — derive a badged copy of an endpoint capability.
 *
 * Creates a copy of src_ep in dest_cnode[dest_slot] with the given badge
 * value.  The badge value encodes the sender's identity; the receiver reads
 * it from seL4_MessageInfo_get_badge().
 *
 * Parameters:
 *   src_ep       source endpoint capability to badge
 *   badge        badge value to embed (caller-defined)
 *   dest_cnode   CNode in which to place the minted cap
 *   dest_slot    slot index within dest_cnode
 *   dest_depth   CNode depth (bits) — typically the PD's cnode_size_bits
 *
 * Returns seL4_NoError on success, non-zero seL4_Error otherwise.
 */
seL4_Error ep_mint_badge(seL4_CPtr  src_ep,
                         seL4_Word  badge,
                         seL4_CPtr  dest_cnode,
                         seL4_Word  dest_slot,
                         uint32_t   dest_depth);

/*
 * ep_find_by_service_id — look up a previously-allocated endpoint by service ID.
 *
 * The ep_alloc layer maintains an internal table mapping service IDs to
 * endpoint capabilities.  ep_alloc_for_service() both allocates a new
 * endpoint and registers it under service_id.
 *
 * Returns seL4_CapNull if the service_id has not been registered.
 */
seL4_CPtr ep_find_by_service_id(uint16_t service_id);

/*
 * ep_alloc_for_service — allocate an endpoint and register it under service_id.
 *
 * If an endpoint is already registered for service_id, the existing cap
 * is returned without allocating a new one.
 *
 * Returns seL4_CapNull on allocation failure.
 */
seL4_CPtr ep_alloc_for_service(uint16_t service_id);
