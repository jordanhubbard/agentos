/*
 * cap-audit/interface.h — Capability Audit IPC Contract
 *
 * Service:  Root task (ring 1) — implemented in cap_audit.c
 * Version:  1
 *
 * Provides two privileged operations for enumerating all capability slots
 * held by any protection domain or by a specific vibeOS guest instance.
 * Both operations are restricted to the controller PD (badge check:
 * badge_client_id == CONTROLLER_CLIENT_ID).
 *
 * IPC transport:
 *   - Caller fills a sel4_msg_t with the opcode and arguments.
 *   - Result count is returned in rep.data[0..3] (uint32_t, little-endian).
 *   - Audit entries are written to the shared audit memory region
 *     (virtual address g_audit_mr_vaddr, mapped at boot).
 *   - Both operations return SEL4_ERR_OK on success or a SEL4_ERR_* code
 *     on failure (see below).
 *
 * Wire-format compatibility:
 *   Callers must tolerate extra trailing bytes in cap_audit_entry_t;
 *   the _pad field is reserved and must be zero on send.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define CAP_AUDIT_CONTRACT_VERSION  UINT32_C(1)

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

/*
 * OP_CAP_AUDIT (0xCA01) — enumerate capabilities across all (or one) PD(s).
 *
 * Request layout (sel4_msg_t.data[]):
 *   data[0..3]  uint32_t pd_id   — PD index to filter; 0 = all PDs
 *
 * Reply layout (sel4_msg_t.data[]):
 *   data[0..3]  uint32_t count   — number of cap_audit_entry_t entries written
 *                                  to the shared audit MR
 *
 * Access control:
 *   Caller badge must have client_id == CONTROLLER_CLIENT_ID.
 *   Returns SEL4_ERR_PERM_DENIED if the badge check fails.
 */
#define OP_CAP_AUDIT            UINT32_C(0xCA01)

/*
 * OP_CAP_AUDIT_GUEST (0xCA02) — enumerate capabilities held by one guest.
 *
 * Request layout (sel4_msg_t.data[]):
 *   data[0..3]  uint32_t handle  — vos_handle_t identifying the guest instance
 *
 * Reply layout (sel4_msg_t.data[]):
 *   data[0..3]  uint32_t count   — number of cap_audit_entry_t entries written
 *
 * Access control:
 *   Same badge check as OP_CAP_AUDIT.
 *   Returns SEL4_ERR_INVALID_CAP if vos_instance_get(handle) returns NULL.
 */
#define OP_CAP_AUDIT_GUEST      UINT32_C(0xCA02)

/* ── Controller client-ID constant ──────────────────────────────────────── */

/*
 * Badge encoding used throughout the root-task server loop:
 *
 *   bits[63:48]  service_id   (upper 16 bits)
 *   bits[47:32]  client_id    (next 16 bits)
 *   bits[31:0]   pd_index     (lower 32 bits)  [optional, may be zero]
 *
 * The controller PD is always assigned client_id 0 in the badge minting
 * step of main.c (ep_mint_badge with client_id==0).  Any caller whose
 * badge[47:32] != CONTROLLER_CLIENT_ID must be rejected.
 */
#define CONTROLLER_CLIENT_ID    UINT32_C(0)

/* Badge extraction helper (works on the uint64_t badge value) */
#define BADGE_CLIENT_ID(badge)  (uint32_t)(((badge) >> 32u) & 0xFFFFu)

/* ── Audit entry structure ────────────────────────────────────────────────── */

/*
 * cap_audit_entry_t — one capability slot record in the audit result.
 *
 * Written sequentially into the shared audit memory region starting at
 * offset 0.  The caller must have mapped a region large enough for the
 * expected entry count before invoking OP_CAP_AUDIT.
 *
 * Fields:
 *   pd_id      PD index that owns this capability (matches system_desc_t.pds[])
 *   cslot      seL4 capability slot index (CPtr / CNode slot)
 *   cap_type   seL4 object type constant (seL4_TCBObject, seL4_EndpointObject,
 *              seL4_UntypedObject, seL4_ARM_VSpaceObject, …)
 *   revocable  1 if the cap has a parent (can be revoked via seL4_CNode_Revoke),
 *              0 if it is a root-level initial capability
 *   _pad       reserved — must be zero; do not interpret
 *   name       NUL-terminated debug label copied from cap_node_t.name[]
 *              (or cap_acct_entry_t.name[] when walking the flat table);
 *              truncated to 15 chars + NUL if the source label is longer
 */
typedef struct __attribute__((packed)) {
    uint32_t pd_id;       /* protection domain index                    */
    uint32_t cslot;       /* seL4 capability slot (CPtr)                */
    uint32_t cap_type;    /* seL4 object type constant                  */
    uint8_t  revocable;   /* 1 = has parent (revocable), 0 = root cap   */
    uint8_t  _pad[3];     /* reserved; zero on transmit                 */
    char     name[16];    /* NUL-terminated label (max 15 chars + NUL)  */
} cap_audit_entry_t;

_Static_assert(sizeof(cap_audit_entry_t) == 32,
               "cap_audit_entry_t must be exactly 32 bytes");

/* ── Error codes (subset of SEL4_ERR_* used by this contract) ────────────── */

/*
 * These are aliases for the standard SEL4_ERR_* constants defined in
 * sel4_ipc.h.  They are repeated here so contract consumers do not need to
 * include sel4_ipc.h.
 */
#define CAP_AUDIT_ERR_OK           UINT32_C(0)   /* SEL4_ERR_OK           */
#define CAP_AUDIT_ERR_PERM_DENIED  UINT32_C(9)   /* SEL4_ERR_FORBIDDEN    */
#define CAP_AUDIT_ERR_INVALID_CAP  UINT32_C(2)   /* SEL4_ERR_NOT_FOUND    */
