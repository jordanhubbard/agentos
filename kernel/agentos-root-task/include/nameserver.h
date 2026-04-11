/*
 * agentOS NameServer — Public Interface
 *
 * The NameServer is a passive PD that provides runtime service discovery.
 * It decouples callers from hardcoded channel IDs and provides a liveness
 * registry that future services (SpawnServer, AppManager) build on top of.
 *
 * Usage pattern:
 *   // At boot, controller pre-registers known services:
 *   microkit_mr_set(0, OP_NS_REGISTER);
 *   microkit_mr_set(1, CH_AGENTFS);          // channel_id callers use
 *   microkit_mr_set(2, TRACE_PD_AGENTFS);    // pd_id
 *   microkit_mr_set(3, CAP_CLASS_FS);        // cap_classes provided
 *   microkit_mr_set(4, 1);                   // version
 *   ns_pack_name(NS_SVC_AGENTFS, 5);         // name into MR5..MR8
 *   microkit_ppcall(CH_NAMESERVER, microkit_msginfo_new(0, 9));
 *
 *   // Any PD looks up a service:
 *   microkit_mr_set(0, OP_NS_LOOKUP);
 *   ns_pack_name("agentfs", 1);              // name into MR1..MR4
 *   microkit_msginfo r = microkit_ppcall(CH_NAMESERVER, microkit_msginfo_new(0, 5));
 *   if (microkit_mr_get(0) == NS_OK) {
 *       uint32_t ch = microkit_mr_get(1);    // channel to use
 *       uint32_t status = microkit_mr_get(3); // NS_STATUS_READY etc.
 *   }
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Name encoding ───────────────────────────────────────────────────────────
 *
 * Names are up to 31 bytes + null terminator = 32 bytes total.
 * Packed into 4 × seL4_Word MRs on 64-bit targets (8 bytes per MR).
 * Byte order: little-endian within each word (byte 0 in bits 7:0).
 *
 * Helper macros use uintptr_t (= seL4_Word width on target platform).
 * On 32-bit targets each MR holds 4 bytes → 8 MRs needed; pass mr_start
 * accordingly.  All existing PDs are 64-bit so 4 MRs is the standard.
 */
#define NS_NAME_MAX     32   /* bytes including null terminator */
#define NS_NAME_MR_COUNT 4   /* seL4_Words needed to carry NS_NAME_MAX bytes */

/* ── Service status ──────────────────────────────────────────────────────── */
#define NS_STATUS_UNKNOWN   0u  /* registered but init() not yet signalled */
#define NS_STATUS_READY     1u  /* running and healthy */
#define NS_STATUS_DEGRADED  2u  /* running but impaired */
#define NS_STATUS_OFFLINE   3u  /* shut down or crashed */

/* ── Opcodes (MR0 in PPC calls to NameServer) ────────────────────────────── */
#define OP_NS_REGISTER       0xD0u  /* register a service */
/* DEPRECATED: use OP_NS_LOOKUP_GATED — no authorization check */
#define OP_NS_LOOKUP         0xD1u  /* look up service by name (no auth) */
#define OP_NS_UPDATE_STATUS  0xD2u  /* update liveness status */
#define OP_NS_LIST           0xD3u  /* dump all entries to shmem */
#define OP_NS_DEREGISTER     0xD4u  /* remove a service registration */
#define OP_NS_HEALTH         0xD5u  /* NameServer health check */
/*
 * OP_NS_LOOKUP_GATED — authorized lookup using badge-encoded policy.
 *
 * The badge of the incoming PPC encodes:
 *   high 16 bits = allowed_cats  : CAP_CLASS_* bitmask the caller is
 *                                  permitted to reach (set by init_agent
 *                                  at bootstrap for each registered PD).
 *   low  16 bits = requester_pd  : TRACE_PD_* of the calling PD.
 *
 * If the target entry's cap_classes do not overlap with allowed_cats
 * the server returns NS_ERR_FORBIDDEN instead of the channel_id.
 * This prevents a compromised worker from reaching services it was never
 * granted access to at spawn time.
 *
 * Call layout identical to OP_NS_LOOKUP (MR1..MR4 = packed name).
 * Reply adds no extra MRs beyond the standard NS_OK reply.
 */
#define OP_NS_LOOKUP_GATED   0xD6u  /* authorized lookup via badge policy */

/* ── Return codes (MR0 in replies) ──────────────────────────────────────── */
#define NS_OK               0u
#define NS_ERR_FULL         1u  /* registry full */
#define NS_ERR_NOT_FOUND    2u  /* name not found */
#define NS_ERR_DUPLICATE    3u  /* name already registered */
#define NS_ERR_UNKNOWN_OP   4u  /* unknown opcode */
#define NS_ERR_BAD_ARGS     5u  /* invalid or empty name */
#define NS_ERR_FORBIDDEN    6u  /* caller's allowed_cats do not match service's cap_classes */

/* ── OP_NS_REGISTER call layout ──────────────────────────────────────────
 *   MR0 = OP_NS_REGISTER
 *   MR1 = channel_id   (callers use this to reach the service)
 *   MR2 = pd_id        (TRACE_PD_* constant)
 *   MR3 = cap_classes  (CAP_CLASS_* bitmask of what this service provides)
 *   MR4 = version      (service version integer)
 *   MR5..MR8 = name    (NS_NAME_MAX bytes packed, 4 × seL4_Word)
 * Reply:
 *   MR0 = NS_OK | NS_ERR_*
 */

/* ── OP_NS_LOOKUP call layout ────────────────────────────────────────────
 *   MR0 = OP_NS_LOOKUP
 *   MR1..MR4 = name    (NS_NAME_MAX bytes packed, 4 × seL4_Word)
 * Reply (on NS_OK):
 *   MR0 = NS_OK
 *   MR1 = channel_id
 *   MR2 = pd_id
 *   MR3 = status       (NS_STATUS_*)
 *   MR4 = cap_classes
 *   MR5 = version
 */

/* ── OP_NS_UPDATE_STATUS call layout ─────────────────────────────────────
 *   MR0 = OP_NS_UPDATE_STATUS
 *   MR1 = channel_id   (identifies the service to update)
 *   MR2 = new_status   (NS_STATUS_*)
 * Reply:
 *   MR0 = NS_OK | NS_ERR_NOT_FOUND
 */

/* ── OP_NS_LIST call layout ──────────────────────────────────────────────
 *   MR0 = OP_NS_LIST
 *   (no other input MRs)
 * Reply:
 *   MR0 = NS_OK
 *   MR1 = entry_count
 *   (entries written to ns_registry_shmem — controller-mapped region)
 */

/* ── OP_NS_DEREGISTER call layout ────────────────────────────────────────
 *   MR0 = OP_NS_DEREGISTER
 *   MR1 = channel_id
 * Reply:
 *   MR0 = NS_OK | NS_ERR_NOT_FOUND
 */

/* ── OP_NS_HEALTH call layout ────────────────────────────────────────────
 *   MR0 = OP_NS_HEALTH
 * Reply:
 *   MR0 = NS_OK
 *   MR1 = registered_count
 *   MR2 = NS_VERSION
 */

/* ── Channel IDs (from each caller's perspective) ────────────────────────
 * These must match the channel assignments in agentos.system.
 */
#define CH_NAMESERVER              18  /* controller → nameserver */
#define CH_NAMESERVER_FROM_INIT     7  /* init_agent → nameserver */
#define CH_NAMESERVER_FROM_WORKER  12  /* any worker → nameserver */

/* ── Well-known service name constants ───────────────────────────────────
 * Use these instead of raw strings to avoid typos.
 */
#define NS_SVC_EVENTBUS     "event_bus"
#define NS_SVC_AGENTFS      "agentfs"
#define NS_SVC_CAPSTORE     "capstore"
#define NS_SVC_VIBEENGINE   "vibe_engine"
#define NS_SVC_CONSOLEMUX   "console_mux"
#define NS_SVC_NETISOLATOR  "net_isolator"
/* Future services — reserved names for tasks 2–7 */
#define NS_SVC_VFS          "vfs"
#define NS_SVC_NET          "net"
#define NS_SVC_DRIVERBUS    "driver_bus"
#define NS_SVC_SPAWN        "spawn"
#define NS_SVC_APPMANAGER   "app_manager"
#define NS_SVC_HTTP         "http"

/* ── NameServer version ───────────────────────────────────────────────── */
#define NS_VERSION          1u

/* ── shmem list dump layout (ns_registry_shmem, written by NameServer) ──
 *
 * Used by OP_NS_LIST.  Controller maps this region read-only and reads
 * entries after calling OP_NS_LIST.  Other PDs do NOT map this region.
 *
 * Layout:
 *   [0..3]   magic    = NS_LIST_MAGIC
 *   [4..7]   count    = number of valid entries
 *   [8..15]  _pad     = 0
 *   [16..]   entries  = ns_list_entry_t array
 *
 * Total for 64 entries: 16 + 64×64 = 4112 bytes < 8KB shmem region.
 */
#define NS_LIST_MAGIC    0x4E534C53u  /* "NSLS" */

typedef struct {
    char     name[NS_NAME_MAX];  /* null-terminated service name   */
    uint32_t channel_id;         /* caller's channel to the service */
    uint32_t pd_id;              /* TRACE_PD_* value                */
    uint32_t cap_classes;        /* CAP_CLASS_* bitmask             */
    uint32_t version;            /* service version                 */
    uint8_t  status;             /* NS_STATUS_*                     */
    uint8_t  flags;              /* reserved, always 0              */
    uint8_t  _pad[6];
    uint64_t registered_at;      /* boot tick at registration time  */
} ns_list_entry_t;               /* 64 bytes exactly                */

typedef struct {
    uint32_t       magic;
    uint32_t       count;
    uint8_t        _pad[8];
    ns_list_entry_t entries[/* count */];
} ns_list_header_t;

/* ── Name packing helpers (inline, no-alloc) ─────────────────────────────
 *
 * ns_pack_name(name, mr_start):
 *   Writes NS_NAME_MAX bytes of name into MRs [mr_start .. mr_start+3].
 *   Uses little-endian byte packing (byte 0 in bits 7:0 of first MR).
 *
 * ns_unpack_name(buf, mr_start):
 *   Reads NS_NAME_MAX bytes from MRs [mr_start .. mr_start+3] into buf.
 *   Always null-terminates buf at offset NS_NAME_MAX-1.
 *
 * These are static inline so any PD that includes this header can use
 * them without a separate compilation unit.
 */
#include <microkit.h>  /* microkit_mr_set / microkit_mr_get */

static inline void ns_pack_name(const char *name, int mr_start)
{
    for (int w = 0; w < NS_NAME_MR_COUNT; w++) {
        uintptr_t word = 0;
        for (int b = 0; b < (int)sizeof(uintptr_t); b++) {
            int idx = w * (int)sizeof(uintptr_t) + b;
            if (idx < NS_NAME_MAX && name[idx] != '\0') {
                word |= ((uintptr_t)(unsigned char)name[idx]) << (b * 8);
            } else if (idx < NS_NAME_MAX) {
                /* null terminator — remaining bytes stay zero */
                break;
            }
        }
        microkit_mr_set(mr_start + w, word);
    }
}

static inline void ns_unpack_name(char *buf, int mr_start)
{
    for (int w = 0; w < NS_NAME_MR_COUNT; w++) {
        uintptr_t word = microkit_mr_get(mr_start + w);
        for (int b = 0; b < (int)sizeof(uintptr_t); b++) {
            int idx = w * (int)sizeof(uintptr_t) + b;
            if (idx < NS_NAME_MAX) {
                buf[idx] = (char)((word >> (b * 8)) & 0xFF);
            }
        }
    }
    buf[NS_NAME_MAX - 1] = '\0';
}
