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

#define PD_MAX_NAME_LEN    32u  /* max length of pd_desc_t.name (with NUL)   */
#define PD_MAX_ELF_PATH    64u  /* max length of pd_desc_t.elf_path (NUL)    */
#define PD_MAX_INIT_EPS     8u  /* max initial endpoints distributed per PD  */
#define SYSTEM_MAX_PDS     64u  /* max PDs per system description             */

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
 */
typedef struct {
    const char   name[PD_MAX_NAME_LEN];
    const char   elf_path[PD_MAX_ELF_PATH];
    uint32_t     stack_size;
    uint32_t     cnode_size_bits;
    uint8_t      priority;
    uint32_t     init_ep_count;
    pd_init_ep_t init_eps[PD_MAX_INIT_EPS];
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

/* Standard per-PD CNode slot assignments for well-known capabilities.
 * These are the slots at which each PD finds its initial endpoint caps. */
#define PD_CNODE_SLOT_NAMESERVER_EP   0u
#define PD_CNODE_SLOT_SERIAL_EP       1u
#define PD_CNODE_SLOT_EVENTBUS_EP     2u
#define PD_CNODE_SLOT_CAP_BROKER_EP   3u
#define PD_CNODE_SLOT_LOG_DRAIN_EP    4u
