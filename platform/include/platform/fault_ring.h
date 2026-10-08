/*
 * agentOS fault-ring shared-memory layout
 *
 * The fault_handler PD (services/fault-handler/fault_handler.c, docs/TCB.md)
 * logs every seL4 fault it is handed into a ring buffer living in a private
 * RAM region that the ROOT TASK provisions and maps into that PD's VSpace at
 * AOS_FAULT_RING_VA. The PD assigns its `fault_ring_vaddr` global from this
 * constant; nothing else may write that global on target.
 *
 * Before this header existed, `fault_ring_vaddr` was a plain .bss global that
 * nothing in the tree ever assigned, so fault_handler stored its ring header
 * through a NULL pointer on its very first instruction after entry and died
 * before printing anything. This header is the single place the root task and
 * the PD agree on where that region lives.
 *
 * WHAT THIS IS NOT. The region is ordinary private RAM, not a shared sDDF
 * queue: no other PD maps it, and it conveys no device, IRQ or guest
 * authority. Reading the ring is an IPC operation on fault_handler's own
 * endpoint (OP_FAULT_DUMP), not a second mapping.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef AOS_PLATFORM_FAULT_RING_H
#define AOS_PLATFORM_FAULT_RING_H

#include <stdint.h>

/*
 * Virtual address of the fault-ring region inside fault_handler's VSpace.
 *
 * Chosen to be 2 MiB-aligned and clear of every other fixed VA this PD sees:
 * its ELF image at 0x400000, the IPC buffer at 0x10000000, the log config and
 * client pages at 0x1000a000 / 0x1000b000 (platform/log_ring.h) and the
 * authority boot page at 0x1000c000 (platform/authority.h). It is also clear
 * of the 0x2a000000-0x30200000 band that the serial, framebuffer, display and
 * event-bus regions occupy in other PDs, so moving a PD's role later cannot
 * collide with it.
 */
#define AOS_FAULT_RING_VA        UINT64_C(0x32000000)

/*
 * One 2 MiB large page backs the region. The ring itself uses only the first
 * AOS_FAULT_RING_BYTES; the remainder is mapped but unused, which keeps the
 * grant to exactly one frame capability and therefore keeps the published
 * authority row (platform/authority.h) a fixed, assertable number.
 */
#define AOS_FAULT_RING_REGION    UINT64_C(0x200000)

/* Ring size: 64-byte header + 48-byte entries, >5000 entries. */
#define AOS_FAULT_RING_BYTES     UINT64_C(0x40000)

#endif /* AOS_PLATFORM_FAULT_RING_H */
