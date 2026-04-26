/*
 * sel4_msg_types.h — minimal sel4_msg_t / sel4_badge_t / SEL4_ERR_* types
 *
 * Defines only the agentOS IPC message types and error codes, with no
 * dependency on <sel4/sel4.h> or any other seL4 kernel header.  This
 * allows headers (like cap_audit.h) that are included in root-task
 * compilation units — which define their own seL4 type stubs via
 * sel4_boot.h — to use these types without triggering redefinition
 * conflicts with the real seL4 SDK headers.
 *
 * sel4_ipc.h includes this header and adds the seL4 IPC primitives on top.
 * Service PDs should include sel4_ipc.h directly; root-task headers that
 * only need the message types may include this header instead.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ── Message geometry ────────────────────────────────────────────────────── */

#define SEL4_MSG_DATA_BYTES  48u

/* ── Error / status codes ────────────────────────────────────────────────── */

#define SEL4_ERR_OK           0u
#define SEL4_ERR_INVALID_OP   1u
#define SEL4_ERR_NOT_FOUND    2u
#define SEL4_ERR_PERM         3u
#define SEL4_ERR_BAD_ARG      4u
#define SEL4_ERR_NO_MEM       5u
#define SEL4_ERR_BUSY         6u
#define SEL4_ERR_OVERFLOW     7u
#define SEL4_ERR_INTERNAL     8u
#define SEL4_ERR_FORBIDDEN    9u

/* ── Core message type ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[SEL4_MSG_DATA_BYTES];
} sel4_msg_t;

_Static_assert(sizeof(sel4_msg_t) == 4u + 4u + SEL4_MSG_DATA_BYTES,
               "sel4_msg_t layout mismatch");

/* ── Badge type ──────────────────────────────────────────────────────────── */

typedef uint64_t sel4_badge_t;

/* ── Inline IPC helpers (used by all service PDs) ────────────────────────── *
 * Guarded so files that define their own versions don't get redefinitions.
 */
#ifndef AGENTOS_IPC_HELPERS_DEFINED
#define AGENTOS_IPC_HELPERS_DEFINED
static inline uint32_t msg_u32(const sel4_msg_t *m, uint32_t off) {
    if (off + 4u > SEL4_MSG_DATA_BYTES) return 0u;
    uint32_t v; __builtin_memcpy(&v, m->data + off, 4u); return v;
}
static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u > SEL4_MSG_DATA_BYTES) return;
    __builtin_memcpy(m->data + off, &v, 4u);
}
#endif /* AGENTOS_IPC_HELPERS_DEFINED */

/* Convenience macro: declare local stub buffers for functions that use
 * rep_u32/msg_u32 but do not receive rep/req as parameters.            */
#ifndef IPC_STUB_LOCALS
#define IPC_STUB_LOCALS \
    sel4_msg_t _rep_buf = {0}; sel4_msg_t *rep = &_rep_buf; \
    sel4_msg_t _req_dummy = {0}; const sel4_msg_t *req = &_req_dummy; (void)req;
#endif
