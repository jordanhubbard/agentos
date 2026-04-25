/*
 * system_desc.h — compile-time system topology descriptor for the root task
 *
 * Defines the static system description passed to the root task boot sequence.
 * One system_desc_t is provided per supported board/architecture in the
 * corresponding system_desc_<arch>.c file.
 *
 * The system description lists every protection domain (PD) that the root task
 * must create, configure, and start during boot.  It also lists the initial
 * endpoints that each PD should receive minted copies of.
 *
 * Ordering within system_desc_t.pds[] is significant: the root task starts PDs
 * in array order.  The nameserver (pd[0]) must be first so that subsequent PDs
 * can register with it before they begin normal operation.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ── Maximum table sizes ──────────────────────────────────────────────────── */

#define PD_MAX_NAME_LEN       32u  /* max length of pd_desc_t.name (with NUL)   */
#define PD_MAX_ELF_PATH       64u  /* max length of pd_desc_t.elf_path (NUL)    */
#define PD_MAX_INIT_EPS        8u  /* max initial endpoints distributed per PD  */
#define PD_MAX_IRQS            8u  /* max hardware IRQs bound per PD            */
#define PD_MAX_DEVICE_FRAMES   4u  /* max MMIO device frames distributed per PD */
#define PD_MAX_MEMORY_REGIONS  2u  /* max large RAM regions mapped per PD       */
#define SYSTEM_MAX_PDS        64u  /* max PDs per system description             */

/*
 * PD_IRQHANDLER_SLOT_BASE — first CNode slot in a PD's own CNode reserved for
 * seL4 IRQ handler capabilities distributed by the root task during boot.
 *
 * Slot layout within the PD's CNode (cnode_size_bits == 6 → 64 slots):
 *   Slots  0 .. PD_MAX_INIT_EPS-1   — initial endpoint caps (pd_init_ep_t)
 *   Slots 64 .. 64+PD_MAX_IRQS-1   — IRQ handler caps (one per irq_desc_t)
 *
 * 64 is chosen to sit well above the maximum init_ep_count (PD_MAX_INIT_EPS=8)
 * and well below the practical CNode capacity (2^6=64 or larger).  PDs that
 * need IRQ handler caps must be created with cnode_size_bits >= 7 (128 slots).
 */
#define PD_IRQHANDLER_SLOT_BASE  64u

/* ── memory_region_desc_t ────────────────────────────────────────────────── */

/*
 * memory_region_desc_t — one large anonymous RAM region to map into a PD.
 *
 * The root task allocates 2 MB large pages from the untyped pool and maps them
 * into the PD's VSpace at [vaddr, vaddr+size).  Both vaddr and size must be
 * 2 MB-aligned (multiples of 2097152).
 *
 * After mapping, the PD can read/write the region like normal RAM.  This is
 * used to give VMM PDs their guest RAM window (e.g. 256 MB at 0x40000000 on
 * AArch64 for linux_vmm's guest_ram_vaddr).
 *
 * Fields:
 *   vaddr     target virtual address in the PD VSpace (2 MB aligned)
 *   size      total size in bytes (2 MB aligned; must be >= 2 MB)
 *   writable  1 = read/write, 0 = read-only
 *   name      human-readable label (for diagnostics)
 */
typedef struct {
    uint64_t    vaddr;    /* target VA (2 MB aligned)   */
    uint32_t    size;     /* region size (2 MB aligned) */
    uint8_t     writable; /* 1 = R/W, 0 = R/O           */
    char        name[19]; /* debug label                */
} memory_region_desc_t;

/* ── device_frame_desc_t ─────────────────────────────────────────────────── */

/*
 * device_frame_desc_t — one MMIO device frame to map into a PD's CSpace.
 *
 * The root task looks up a device-memory untyped cap covering paddr, retypes
 * it into a frame, and mints a copy into the PD's CNode at cnode_slot.
 * The PD then maps the frame at a chosen VA to access the MMIO registers.
 *
 * Fields:
 *   paddr       physical base address of the device MMIO region (page-aligned)
 *   size_bits   log2 of the region size (e.g. 12 for 4 KB, 16 for 64 KB)
 *   cnode_slot  slot in the PD's CNode where the frame cap lands
 *   name        human-readable label for diagnostics (e.g. "virtio-net")
 */
typedef struct __attribute__((packed)) {
    uint64_t paddr;       /* physical address of device MMIO region          */
    uint8_t  size_bits;   /* log2 of region size in bytes                    */
    uint8_t  cnode_slot;  /* destination slot in the PD's own CNode          */
    char     name[14];    /* human-readable label                            */
} device_frame_desc_t;

/* ── irq_desc_t ───────────────────────────────────────────────────────────── */

/*
 * irq_desc_t — compile-time descriptor for one hardware IRQ bound to a PD.
 *
 * The root task calls seL4_IRQControl_Get during boot to obtain an IRQ handler
 * capability for each irq_desc_t entry and places it into the PD's CNode at
 * slot (PD_IRQHANDLER_SLOT_BASE + index).
 *
 * Fields:
 *   irq_number   hardware IRQ number (GIC SPI number on AArch64)
 *   ntfn_badge   badge value placed on the PD's notification object for this
 *                IRQ; the PD dispatches on this badge in its notification loop
 *   name         human-readable label for diagnostics (e.g. "virtio-net")
 *
 * Size: 4 + 4 + 16 = 24 bytes (packed).
 */
typedef struct __attribute__((packed)) {
    uint32_t irq_number;  /* hardware IRQ number                        */
    uint32_t ntfn_badge;  /* badge on the PD's notification for this IRQ */
    char     name[16];    /* human-readable label, e.g. "virtio-net"    */
} irq_desc_t;

_Static_assert(sizeof(irq_desc_t) == 24u,
               "irq_desc_t must be exactly 24 bytes");

/* ── pd_init_ep_t ─────────────────────────────────────────────────────────── */

/*
 * pd_init_ep_t — one initial endpoint capability to mint into a PD's CNode.
 *
 * Fields:
 *   service_id   identifies which service endpoint to badge and distribute;
 *                matches the service_id used by ep_alloc_for_service()
 *   cnode_slot   the slot within the PD's own CNode where the minted cap lands
 */
typedef struct {
    uint16_t service_id;  /* service this endpoint belongs to */
    uint16_t cnode_slot;  /* destination slot in the PD's CNode */
} pd_init_ep_t;

/* ── pd_desc_t ────────────────────────────────────────────────────────────── */

/*
 * pd_desc_t — compile-time descriptor for one protection domain.
 *
 * Fields:
 *   name             human-readable PD name (for diagnostics and ELF lookup)
 *   elf_path         path/name used to locate the embedded ELF in BootInfo
 *   stack_size       desired stack size in bytes (must be page-aligned)
 *   cnode_size_bits  log2 of the number of slots in the PD's own CNode
 *   priority         seL4 scheduling priority (0 = lowest, 255 = highest)
 *   init_ep_count    number of entries in init_eps[]
 *   init_eps         initial endpoint capabilities distributed to this PD
 *   irq_count        number of hardware IRQs bound to this PD (0 = none)
 *   irqs             hardware IRQ descriptors; root task binds these at boot
 *                    and places handler caps at PD_IRQHANDLER_SLOT_BASE+i
 */
typedef struct {
    const char         name[PD_MAX_NAME_LEN];
    const char         elf_path[PD_MAX_ELF_PATH];
    uint32_t           stack_size;
    uint32_t           cnode_size_bits;
    uint8_t            priority;
    uint32_t           init_ep_count;
    pd_init_ep_t       init_eps[PD_MAX_INIT_EPS];
    uint8_t            irq_count;
    irq_desc_t         irqs[PD_MAX_IRQS];
    uint8_t            device_frame_count;
    device_frame_desc_t device_frames[PD_MAX_DEVICE_FRAMES];
    uint8_t            mr_count;
    memory_region_desc_t memory_regions[PD_MAX_MEMORY_REGIONS];
} pd_desc_t;

/* ── system_desc_t ────────────────────────────────────────────────────────── */

/*
 * system_desc_t — top-level system topology descriptor.
 *
 * Fields:
 *   pd_count  number of entries in pds[]
 *   pds       array of per-PD descriptors, in start order
 */
typedef struct {
    uint32_t        pd_count;
    const pd_desc_t pds[SYSTEM_MAX_PDS];
} system_desc_t;

/* ── Well-known service IDs ───────────────────────────────────────────────── */

/*
 * Service IDs used in pd_init_ep_t.service_id.
 * These values are stable and must not be renumbered without updating all
 * system description tables.
 */
#define SVC_ID_NAMESERVER      1u
#define SVC_ID_SERIAL          2u
#define SVC_ID_EVENTBUS        3u
#define SVC_ID_VIBE_ENGINE     4u
#define SVC_ID_CAP_BROKER      5u
#define SVC_ID_LOG_DRAIN       6u
#define SVC_ID_VFS_SERVER      7u
#define SVC_ID_NET_SERVER      8u
#define SVC_ID_BLOCK_SERVICE   9u
#define SVC_ID_TIMER          10u
#define SVC_ID_INIT_AGENT     11u
#define SVC_ID_IRQ_PD         12u
#define SVC_ID_TIMER_PD       13u
#define SVC_ID_NET_PD         14u   /* virtio-net driver PD                  */
#define SVC_ID_FB_PD          15u   /* framebuffer PD                        */
#define SVC_ID_LINUX_VMM      16u   /* Linux VMM PD                          */
#define SVC_ID_FREEBSD_VMM    17u   /* FreeBSD VMM PD                        */

/* Standard per-PD CNode slot assignments for well-known capabilities.
 * These are the slots at which each PD finds its initial endpoint caps. */
#define PD_CNODE_SLOT_NAMESERVER_EP   0u
#define PD_CNODE_SLOT_SERIAL_EP       1u
#define PD_CNODE_SLOT_EVENTBUS_EP     2u
#define PD_CNODE_SLOT_CAP_BROKER_EP   3u
#define PD_CNODE_SLOT_LOG_DRAIN_EP    4u
