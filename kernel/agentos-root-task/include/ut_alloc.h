/*
 * ut_alloc.h — seL4 untyped memory allocator for root-task boot
 *
 * During boot the root task holds a collection of untyped memory capabilities
 * provided by the seL4 kernel via the BootInfo frame.  ut_alloc() carves a
 * typed kernel object out of that pool and places the resulting capability into
 * a specified CNode slot.
 *
 * Callers (e.g. pd_tcb_create, pd_vspace_create) obtain capabilities to
 * seL4_TCBObject, seL4_EndpointObject, and paging objects via this interface.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "sel4_boot.h"

/*
 * ut_alloc — retype untyped memory into a kernel object.
 *
 * Parameters:
 *   type          seL4 object type (e.g. seL4_TCBObject)
 *   size_bits     log2 of the object size in bytes; 0 for fixed-size objects
 *                 such as TCBs and Endpoints (the kernel ignores this field
 *                 for objects whose size is determined by the type)
 *   dest_cnode    CNode capability slot where the new cap is to be placed
 *                 (normally seL4_CapInitThreadCNode for the root task)
 *   dest_index    index within dest_cnode for the new capability
 *   dest_depth    radix (depth) of dest_cnode in bits (e.g. 64)
 *
 * Returns seL4_NoError on success, an seL4_Error code otherwise.
 *
 * On success the caller holds a capability to the new object at
 * dest_cnode[dest_index].
 */
seL4_Error ut_alloc(uint32_t   type,
                    uint32_t   size_bits,
                    seL4_CPtr  dest_cnode,
                    seL4_Word  dest_index,
                    uint32_t   dest_depth);
