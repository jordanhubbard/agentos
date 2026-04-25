/*
 * ut_alloc.c — seL4 untyped memory allocator for root-task boot
 *
 * Initialised once from seL4_BootInfo.  Maintains a flat table of non-device
 * untyped caps.  Each ut_alloc() call retypes one object out of the current
 * untyped cap; when that cap is exhausted seL4_Untyped_Retype returns
 * seL4_NotEnoughMemory and we advance to the next cap.
 *
 * ut_alloc_cap() maintains a separate slot cursor starting at bi->empty.start.
 * main.c advances the cursor past its static slot reservations (PD objects
 * and the EP pool) before any pd_vspace_create call allocates dynamic caps.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ut_alloc.h"
#include "boot_info.h"

#define UT_MAX_CAPS    230u
#define DEV_UT_MAX     32u

typedef struct {
    seL4_CPtr cap;
    uint8_t   size_bits;
} ut_entry_t;

typedef struct {
    seL4_CPtr cap;
    seL4_Word paddr;
    uint8_t   size_bits;
} dev_ut_entry_t;

static ut_entry_t     g_ut[UT_MAX_CAPS];
static uint32_t       g_ut_count;
static uint32_t       g_ut_next;   /* index of the untyped cap currently in use */

static dev_ut_entry_t g_dev_ut[DEV_UT_MAX];
static uint32_t       g_dev_ut_count;

/* Free slot cursor — tracks bi->empty allocation */
static seL4_Word  g_slot_base;  /* bi->empty.start saved at init time */
static seL4_Word  g_slot_cur;   /* next slot to hand out via ut_alloc_slot() */
static seL4_Word  g_slot_end;   /* bi->empty.end (exclusive) */

void ut_alloc_init(const seL4_BootInfo *bi)
{
    g_ut_count     = 0u;
    g_ut_next      = 0u;
    g_dev_ut_count = 0u;
    g_slot_base    = 0u;
    g_slot_cur     = 0u;
    g_slot_end     = 0u;

    if (!bi) {
        return;
    }

    /* Save free-slot region */
    g_slot_base = bi->empty.start;
    g_slot_cur  = bi->empty.start;
    g_slot_end  = bi->empty.end;

    uint32_t n = (uint32_t)(bi->untyped.end - bi->untyped.start);
    if (n > UT_MAX_CAPS) {
        n = UT_MAX_CAPS;
    }

    for (uint32_t i = 0u; i < n; i++) {
        const seL4_UntypedDesc *d = &bi->untypedList[i];
        if (d->isDevice) {
            if (g_dev_ut_count < DEV_UT_MAX) {
                g_dev_ut[g_dev_ut_count].cap       = bi->untyped.start + (seL4_CPtr)i;
                g_dev_ut[g_dev_ut_count].paddr     = d->paddr;
                g_dev_ut[g_dev_ut_count].size_bits = d->sizeBits;
                g_dev_ut_count++;
            }
            continue;
        }
        g_ut[g_ut_count].cap       = bi->untyped.start + (seL4_CPtr)i;
        g_ut[g_ut_count].size_bits = d->sizeBits;
        g_ut_count++;
        if (g_ut_count >= UT_MAX_CAPS) {
            break;
        }
    }
}

seL4_Word ut_free_slot_base(void)
{
    return g_slot_base;
}

void ut_advance_slot_cursor(seL4_Word n_slots)
{
    g_slot_cur += n_slots;
    /* Guard against overflow past the empty region */
    if (g_slot_cur > g_slot_end) {
        g_slot_cur = g_slot_end;
    }
}

seL4_Word ut_alloc_slot(void)
{
    if (g_slot_cur >= g_slot_end) {
        return seL4_CapNull;
    }
    return g_slot_cur++;
}

seL4_Error ut_alloc(uint32_t   type,
                    uint32_t   size_bits,
                    seL4_CPtr  dest_cnode,
                    seL4_Word  dest_index,
                    uint32_t   dest_depth)
{
    (void)dest_depth;   /* depth is encoded in the dest_cnode path; unused here */

    if (g_ut_count == 0u) {
        return seL4_NotEnoughMemory;
    }

    /*
     * Try each untyped cap starting from g_ut_next.  On seL4_NotEnoughMemory
     * advance to the next cap; any other error is fatal for this allocation.
     */
    for (uint32_t i = 0u; i < g_ut_count; i++) {
        uint32_t idx = (g_ut_next + i) % g_ut_count;

        seL4_Error err = seL4_Untyped_Retype(
            g_ut[idx].cap,      /* untyped cap to retype                   */
            (seL4_Word)type,    /* object type                             */
            (seL4_Word)size_bits, /* size in bits (0 for fixed-size objs)  */
            dest_cnode,         /* root: destination CNode cap             */
            0u,                 /* node_index: no CSpace traversal         */
            0u,                 /* node_depth: 0 → root is the target CNode */
            dest_index,         /* node_offset: slot in the target CNode   */
            1u                  /* num_objects                             */
        );

        if (err == seL4_NoError) {
            return seL4_NoError;
        }

        if (err == seL4_NotEnoughMemory) {
            /* Current untyped is exhausted; try the next one */
            g_ut_next = (idx + 1u) % g_ut_count;
            continue;
        }

        /* Any other error (e.g. seL4_InvalidArgument) is non-recoverable */
        return err;
    }

    return seL4_NotEnoughMemory;
}

seL4_Error ut_alloc_cap(uint32_t   type,
                         uint32_t   size_bits,
                         seL4_CPtr *cap_out)
{
    seL4_Word slot = ut_alloc_slot();
    if (slot == seL4_CapNull) {
        *cap_out = seL4_CapNull;
        return seL4_NotEnoughMemory;
    }

    seL4_Error err = ut_alloc(type, size_bits,
                               seL4_CapInitThreadCNode,
                               slot,
                               64u /* dest_depth: root task CNode is 64-bit indexed */);
    if (err != seL4_NoError) {
        /* Slot is wasted; seL4 doesn't allow returning slots to bi->empty */
        *cap_out = seL4_CapNull;
        return err;
    }

    *cap_out = (seL4_CPtr)slot;
    return seL4_NoError;
}

seL4_Error ut_alloc_device_frame(seL4_Word paddr,
                                  seL4_CPtr pd_cnode,
                                  seL4_Word target_slot)
{
    for (uint32_t i = 0u; i < g_dev_ut_count; i++) {
        seL4_Word ut_start = g_dev_ut[i].paddr;
        seL4_Word ut_end   = ut_start + (1UL << g_dev_ut[i].size_bits);
        if (paddr < ut_start || paddr >= ut_end) {
            continue;
        }
        /*
         * Retype this device untyped as a 4K page frame directly into the
         * PD's CNode.  The 64-bit node_depth traversal resolves pd_cnode
         * (a slot in the root task CNode) to get the PD CNode cap, then
         * places the new frame cap at target_slot within that PD CNode.
         */
        return seL4_Untyped_Retype(
            g_dev_ut[i].cap,
            (seL4_Word)seL4_ARM_SmallPageObject,
            0u,                         /* size_bits: ignored for fixed-size page */
            seL4_CapInitThreadCNode,    /* root: root task CNode for traversal    */
            (seL4_Word)pd_cnode,        /* node_index: PD CNode slot in root      */
            64u,                        /* node_depth: 64-bit path into pd_cnode  */
            target_slot,                /* node_offset: slot in PD CNode          */
            1u                          /* num_objects                            */
        );
    }
    return seL4_InvalidArgument;
}
