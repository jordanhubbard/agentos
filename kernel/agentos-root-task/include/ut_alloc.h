/*
 * ut_alloc.h — seL4 untyped memory allocator for root-task boot
 *
 * During boot the root task holds a collection of untyped memory capabilities
 * provided by the seL4 kernel via the BootInfo frame.  ut_alloc() carves a
 * typed kernel object out of that pool and places the resulting capability into
 * a specified CNode slot.
 *
 * ut_alloc_cap() is the preferred high-level interface: it automatically
 * allocates a free CNode slot from the bi->empty region and returns the new
 * capability.  Callers that need explicit slot placement (e.g. for the static
 * PD slot layout in main.c) continue to use ut_alloc() directly.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "sel4_boot.h"
#include "boot_info.h"
#include <stdint.h>

/*
 * ut_alloc_init — seed the allocator from seL4 BootInfo.
 *
 * Must be called once before any other ut_alloc* function.
 * Walks bi->untyped to build the pool of non-device untyped caps.
 * Saves bi->empty.start as the free-slot cursor base; the cursor starts
 * at bi->empty.start and advances on each ut_alloc_slot() call.
 */
void ut_alloc_init(const seL4_BootInfo *bi);

/*
 * ut_free_slot_base — return bi->empty.start saved at init time.
 *
 * Used by main.c to compute the base for its static PD slot layout so that
 * PD_SLOT_CNODE(i), PD_SLOT_TCB(i), etc. fall within the truly-free range.
 */
seL4_Word ut_free_slot_base(void);

/*
 * ut_advance_slot_cursor — advance the dynamic slot cursor by n slots.
 *
 * Called by main.c after computing its static slot layout to ensure that
 * subsequent ut_alloc_cap() calls do not reuse the statically-reserved slots.
 *
 *   ut_advance_slot_cursor(SYSTEM_MAX_PDS * SLOTS_PER_PD + EP_POOL_SIZE);
 */
void ut_advance_slot_cursor(seL4_Word n_slots);

/*
 * ut_alloc_slot — allocate one free CNode slot from the dynamic cursor.
 *
 * Returns seL4_CapNull if bi->empty is exhausted.
 */
seL4_Word ut_alloc_slot(void);

/*
 * ut_alloc — retype untyped memory into a kernel object at an explicit slot.
 *
 * Parameters:
 *   type          seL4 object type (e.g. seL4_TCBObject)
 *   size_bits     log2 of the object size in bytes; 0 for fixed-size objects
 *   dest_cnode    CNode capability where the new cap is to be placed
 *   dest_index    index within dest_cnode for the new capability
 *   dest_depth    radix (depth) of dest_cnode in bits (e.g. 64)
 *
 * Returns seL4_NoError on success, seL4_Error code otherwise.
 */
seL4_Error ut_alloc(uint32_t   type,
                    uint32_t   size_bits,
                    seL4_CPtr  dest_cnode,
                    seL4_Word  dest_index,
                    uint32_t   dest_depth);

/*
 * ut_alloc_cap — retype untyped memory into a kernel object, auto-allocating
 * a free CNode slot from the bi->empty region.
 *
 * On success: *cap_out receives the new capability (== the allocated slot).
 * On failure: *cap_out is set to seL4_CapNull and an error code is returned.
 */
seL4_Error ut_alloc_cap(uint32_t   type,
                         uint32_t   size_bits,
                         seL4_CPtr *cap_out);

/*
 * ut_alloc_device_frame — retype a device untyped covering paddr into a page
 * frame cap and install it directly into a PD's CNode.
 *
 * Parameters:
 *   paddr         physical address of the device MMIO region
 *   pd_cnode      cap slot in root task CNode that holds the PD's CNode cap
 *   target_slot   destination slot within the PD's CNode
 *
 * The function finds the device untyped saved from BootInfo during
 * ut_alloc_init() whose range covers paddr, then calls seL4_Untyped_Retype
 * to place a seL4_ARM_SmallPageObject (4K frame) cap at the specified slot
 * in the PD's CNode via a 64-bit CSpace traversal.
 *
 * Returns seL4_NoError on success, seL4_InvalidArgument if no device untyped
 * covers paddr, or a seL4 kernel error code on retype failure.
 */
seL4_Error ut_alloc_device_frame(seL4_Word paddr,
                                  seL4_CPtr pd_cnode,
                                  seL4_Word target_slot);
