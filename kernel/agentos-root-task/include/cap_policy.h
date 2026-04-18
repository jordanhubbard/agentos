/* cap_policy.h — Runtime capability policy blob format + ring-1 enforcement
 *
 * Two sections:
 *   1. Policy blob format (cap_policy_header_t, cap_grant_t) — loaded from shmem.
 *   2. Ring-1 enforcement API — guards guest VMM IPC and vCPU privilege level.
 *
 * At boot, monitor reads from cap_policy_shmem_vaddr.
 * init_agent can reload via OP_CAP_POLICY_RELOAD.
 *
 * Ring-1 enforcement functions (cap_policy_is_ring0_channel,
 * cap_policy_guest_ipc_check, cap_policy_vcpu_el_check) must be called from
 * the root task IPC dispatch path before any capability is granted.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ─── Policy blob format ─────────────────────────────────────────────────── */

#define CAP_POLICY_MAGIC    0x43415050u  /* "CAPP" */
#define CAP_POLICY_VERSION  1u
#define CAP_POLICY_MAX_GRANTS 128u

typedef struct __attribute__((packed)) {
    uint32_t magic;          /* CAP_POLICY_MAGIC */
    uint32_t version;        /* CAP_POLICY_VERSION */
    uint32_t num_grants;     /* number of cap_grant_t entries following */
    uint32_t _reserved;
} cap_policy_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  agent_id;       /* PD slot index (0=controller, 1=event_bus, ...) */
    uint8_t  cap_class;      /* capability class bitmask (from AGENTOS_CAP_* defines) */
    uint8_t  rights;         /* r=1, w=2, x=4, grant=8, revoke=16 */
    uint8_t  flags;          /* 0x01 = delegatable, 0x02 = revocable */
    uint32_t resource_id;    /* which resource (0 = default for class) */
} cap_grant_t;

/* ─── Ring-1 enforcement ─────────────────────────────────────────────────── */

/*
 * TRACE_PD_* identifiers for VMM protection domains.
 * Follow the sequence in agentos.h (last defined: TRACE_PD_TERM_SERVER = 40).
 */
#define TRACE_PD_LINUX_VMM    41u
#define TRACE_PD_FREEBSD_VMM  42u

/*
 * cap_policy_is_ring0_channel(channel_id)
 *
 * Returns 1 if channel_id is a ring-0 privileged channel that guest VMM PDs
 * must not access.  Returns 0 if channel_id is in the permitted guest set:
 * {CH_SERIAL_PD, CH_NET_PD, CH_BLOCK_PD, CH_USB_PD, CH_FB_PD,
 *  CH_GUEST_PD, CH_VMM_KERNEL}.
 */
int cap_policy_is_ring0_channel(uint32_t channel_id);

/*
 * cap_policy_guest_ipc_check(caller_pd_id, target_channel)
 *
 * Ring-1 enforcement gate called from the root task IPC dispatch path before
 * any capability is granted.  If caller_pd_id is a VMM PD
 * (TRACE_PD_LINUX_VMM or TRACE_PD_FREEBSD_VMM) and target_channel is a
 * ring-0 channel, returns -1 (EPERM).  Returns 0 if permitted.
 */
int cap_policy_guest_ipc_check(uint32_t caller_pd_id, uint32_t target_channel);

/*
 * cap_policy_vcpu_el_check(spsr, is_aarch64)
 *
 * Validate vCPU privilege level before MSG_VMM_VCPU_SET_REGS is applied.
 * AArch64: SPSR.M[3:0] must not be 0x8 (EL2t) or 0x9 (EL2h).
 * x86:     CS.RPL in spsr bits[1:0] must be exactly 0x3 (CPL3); CPL0/1/2 forbidden.
 * Returns 0 if valid, -1 (EPERM) if privilege escalation detected.
 */
int cap_policy_vcpu_el_check(uint64_t spsr, bool is_aarch64);

/* ─── Ring-0 service non-reinvention registry ────────────────────────────── */

/*
 * Function class identifiers for ring-0 service PDs.
 * A VibeOS instance may bind at most one PD per function class.
 * If a PD is already registered for a class, BIND_DEVICE must reuse it.
 */
#define CAP_POLICY_FUNC_CLASS_SERIAL   0x01u
#define CAP_POLICY_FUNC_CLASS_NET      0x02u
#define CAP_POLICY_FUNC_CLASS_BLOCK    0x03u
#define CAP_POLICY_FUNC_CLASS_USB      0x04u
#define CAP_POLICY_FUNC_CLASS_FB       0x05u
#define CAP_POLICY_FUNC_CLASS_MAX      0x05u

/*
 * cap_policy_register_ring0_service(func_class, pd_handle, channel_id)
 *
 * Register a ring-0 service PD as the canonical handler for func_class.
 * Returns 0 on success, -1 if func_class is out of range or already occupied.
 */
int cap_policy_register_ring0_service(uint32_t func_class, uint32_t pd_handle, uint32_t channel_id);

/*
 * cap_policy_find_ring0_service(func_class, out_pd_handle, out_channel_id)
 *
 * Query whether an existing ring-0 service covers func_class.
 * Returns 1 if found (populates *out_pd_handle, *out_channel_id), 0 if none.
 * NULL output pointers are tolerated.
 */
int cap_policy_find_ring0_service(uint32_t func_class, uint32_t *out_pd_handle, uint32_t *out_channel_id);

/*
 * cap_policy_unregister_ring0_service(func_class)
 *
 * Remove a ring-0 service entry.  Safe to call when not registered.
 */
void cap_policy_unregister_ring0_service(uint32_t func_class);
