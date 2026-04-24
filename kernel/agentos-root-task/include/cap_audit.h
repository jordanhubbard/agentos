/*
 * cap_audit.h — OP_CAP_AUDIT / OP_CAP_AUDIT_GUEST handler declarations
 *
 * These functions are wired into the root task server loop in main.c
 * via sel4_server_register (or the equivalent dispatch table call).
 *
 * Both handlers are controller-only: the badge's client_id field must
 * equal CONTROLLER_CLIENT_ID (0) or the call is rejected with
 * SEL4_ERR_FORBIDDEN.
 *
 * The shared audit memory region pointer (g_audit_mr_vaddr) must be
 * initialised before any caller invokes these operations.  Under
 * AGENTOS_TEST_HOST the pointer is set to a static host-side buffer
 * automatically.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

#ifdef AGENTOS_TEST_HOST
/* Host-side stubs for sel4_badge_t and sel4_msg_t */
#ifndef SEL4_WORD_DEFINED
typedef uintptr_t seL4_Word;
#define SEL4_WORD_DEFINED 1
#endif
#ifndef SEL4_BADGE_DEFINED
typedef uint64_t sel4_badge_t;
#define SEL4_BADGE_DEFINED 1
#endif
#ifndef SEL4_MSG_DATA_BYTES
#define SEL4_MSG_DATA_BYTES 48u
#endif
#ifndef SEL4_MSG_T_DEFINED
typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[SEL4_MSG_DATA_BYTES];
} sel4_msg_t;
#define SEL4_MSG_T_DEFINED 1
#endif
#else
#include "sel4_ipc.h"   /* sel4_msg_t, sel4_badge_t */
#endif

#include "../../../contracts/cap-audit/interface.h"

/* ── Handler function prototypes ─────────────────────────────────────────── */

/*
 * handle_cap_audit — handle OP_CAP_AUDIT (0xCA01)
 *
 * Enumerates capabilities across all protection domains (pd_id==0) or
 * a specific PD (pd_id from req->data[0..3]).  Results are written to
 * the shared audit MR; the count is returned in rep->data[0..3].
 *
 * Parameters:
 *   badge  — badge of the incoming IPC (must have CONTROLLER_CLIENT_ID)
 *   req    — incoming request message (opcode + data)
 *   rep    — reply message to fill (opcode + count)
 *   ctx    — reserved; pass NULL
 *
 * Returns:
 *   SEL4_ERR_OK       on success
 *   SEL4_ERR_FORBIDDEN if badge check fails
 */
uint32_t handle_cap_audit(sel4_badge_t       badge,
                           const sel4_msg_t  *req,
                           sel4_msg_t        *rep,
                           void              *ctx);

/*
 * handle_cap_audit_guest — handle OP_CAP_AUDIT_GUEST (0xCA02)
 *
 * Enumerates capabilities held by the vibeOS guest identified by
 * vos_handle_t (from req->data[0..3]).
 *
 * Parameters: same as handle_cap_audit.
 *
 * Returns:
 *   SEL4_ERR_OK        on success
 *   SEL4_ERR_FORBIDDEN if badge check fails
 *   SEL4_ERR_NOT_FOUND if vos_instance_get() returns NULL
 */
uint32_t handle_cap_audit_guest(sel4_badge_t       badge,
                                 const sel4_msg_t  *req,
                                 sel4_msg_t        *rep,
                                 void              *ctx);

/*
 * cap_tree_verify_all_pds — boot-time regression baseline.
 *
 * Called from main.c after all PDs are started.  Logs cap counts per PD
 * to the audit MR (host) or LogSvc (target).  No-op on target in this
 * version; the host-side version populates the audit buffer so tests can
 * inspect the initial state.
 */
void cap_tree_verify_all_pds(void);

/* ── Test-only helpers (AGENTOS_TEST_HOST only) ──────────────────────────── */

#ifdef AGENTOS_TEST_HOST

#include "../../../contracts/cap-audit/interface.h"

/*
 * cap_audit_test_get_entry — read one entry from the host-side audit buffer.
 * cap_audit_test_reset     — zero the host-side audit buffer.
 */
const cap_audit_entry_t *cap_audit_test_get_entry(uint32_t index);
void                     cap_audit_test_reset(void);

#endif /* AGENTOS_TEST_HOST */
