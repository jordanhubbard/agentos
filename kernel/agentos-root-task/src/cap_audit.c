/*
 * cap_audit.c — OP_CAP_AUDIT and OP_CAP_AUDIT_GUEST handlers
 *
 * Implements two privileged security-review operations for the root task's
 * server loop.  Both operations are controller-only: the incoming badge's
 * client_id field must equal CONTROLLER_CLIENT_ID (0).
 *
 * OP_CAP_AUDIT:
 *   Walk the capability accounting table (g_table[] in cap_accounting.c,
 *   accessed via cap_acct_get/cap_acct_count) and write a cap_audit_entry_t
 *   for every entry matching the requested pd_id filter (0 = all PDs) into
 *   the shared audit memory region at g_audit_mr_vaddr.
 *
 * OP_CAP_AUDIT_GUEST:
 *   Look up a vos_instance_t by handle, then walk the capability accounting
 *   table filtering to entries whose pd_index matches the guest handle.
 *   Entries are written to the same audit memory region.
 *
 * cap_tree_verify_all_pds:
 *   Boot-time helper — logs initial cap counts per PD for regression
 *   baseline.  Called from main.c after all PDs are started.
 *
 * Design constraints:
 *   - No libc, no malloc.  All state is static or passed by pointer.
 *   - Under AGENTOS_TEST_HOST the seL4 boot types are replaced by host-side
 *     stubs; the shared MR is replaced by a static host-side buffer.
 *     The test file provides stub implementations of cap_acct_* and
 *     vos_instance_get before including this file.
 *   - Under real seL4 g_audit_mr_vaddr must be set before any caller can
 *     invoke these operations.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../../../contracts/cap-audit/interface.h"

#include <stdint.h>
#include <stddef.h>

/* ── Platform-specific setup ──────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

/*
 * Under AGENTOS_TEST_HOST the following types and constants are expected to
 * have been defined by the including test file before this file is included:
 *
 *   typedef ... vos_handle_t;
 *   typedef struct { ... } vos_instance_t;
 *   typedef struct { ... } cap_acct_entry_t;
 *   vos_instance_t *vos_instance_get(vos_handle_t h);
 *   uint32_t        cap_acct_count(void);
 *   const cap_acct_entry_t *cap_acct_get(uint32_t index);
 *   #define VOS_HANDLE_INVALID ...
 *
 * sel4_badge_t and sel4_msg_t are defined below as host stubs.
 */

/* Error codes (mirror sel4_ipc.h) */
#ifndef SEL4_ERR_OK
#define SEL4_ERR_OK          UINT32_C(0)
#define SEL4_ERR_NOT_FOUND   UINT32_C(2)
#define SEL4_ERR_FORBIDDEN   UINT32_C(9)
#endif

/* sel4_badge_t is uint64_t in sel4_ipc.h */
#ifndef SEL4_BADGE_DEFINED
typedef uint64_t sel4_badge_t;
#define SEL4_BADGE_DEFINED 1
#endif

/* sel4_msg_t stub (mirrors sel4_ipc.h layout) */
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

/* Static audit buffer used in host tests (2048 entries × 32 bytes = 64 KiB) */
#define AUDIT_HOST_BUF_ENTRIES  2048u
static cap_audit_entry_t g_audit_host_buf[AUDIT_HOST_BUF_ENTRIES];

/* g_audit_mr_vaddr points into the static buffer when running on host */
static uintptr_t g_audit_mr_vaddr = (uintptr_t)&g_audit_host_buf[0];

/* Maximum entries the caller can request in one shot (host: bounded) */
#define AUDIT_MAX_ENTRIES  AUDIT_HOST_BUF_ENTRIES

#else /* real seL4 build */

#include "sel4_ipc.h"    /* sel4_msg_t, sel4_badge_t, SEL4_ERR_* */
#include "cap_accounting.h"
#include "vos_create.h"

/*
 * g_audit_mr_vaddr — virtual address of the shared audit memory region.
 *
 * Must be set before the first OP_CAP_AUDIT call.  In a real build the root
 * task maps a frame at this address during the boot sequence; the exact value
 * is determined by the system descriptor's shmem layout.
 */
extern uintptr_t g_audit_mr_vaddr;

/* Maximum entries that fit in the shared MR (4 KiB page / 32 bytes each) */
#define AUDIT_MAX_ENTRIES  (4096u / sizeof(cap_audit_entry_t))

#endif /* AGENTOS_TEST_HOST */

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/*
 * audit_strlcpy — bounded copy with guaranteed NUL termination.
 *
 * Copies at most (max - 1) characters from src to dst, then appends NUL.
 * Equivalent to strlcpy without libc dependency.
 */
static void audit_strlcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0u;
    if (max == 0u) return;
    while (i + 1u < max && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/*
 * write_audit_entry — serialise one cap_acct_entry_t into the audit MR.
 *
 * Parameters:
 *   out       destination pointer into the audit MR
 *   acct      source accounting entry
 *   revocable 1 if the cap has a parent (can be revoked), 0 if root-level
 *
 * Returns a pointer to the next free slot (out + 1) for chaining.
 */
static cap_audit_entry_t *write_audit_entry(cap_audit_entry_t      *out,
                                             const cap_acct_entry_t *acct,
                                             uint8_t                 revocable)
{
    out->pd_id     = acct->pd_index;
    out->cslot     = (uint32_t)(uintptr_t)acct->cap; /* CPtr → uint32_t */
    out->cap_type  = acct->obj_type;
    out->revocable = revocable;
    out->_pad[0]   = 0u;
    out->_pad[1]   = 0u;
    out->_pad[2]   = 0u;
    audit_strlcpy(out->name, acct->name, sizeof(out->name));
    return out + 1;
}

/* ── OP_CAP_AUDIT handler ────────────────────────────────────────────────── */

/*
 * handle_cap_audit — enumerate caps owned by any or all PDs.
 *
 * Badge check: BADGE_CLIENT_ID(badge) must equal CONTROLLER_CLIENT_ID.
 *
 * req->data[0..3]: uint32_t pd_id filter (0 = all PDs, else match exactly).
 * rep->data[0..3]: uint32_t entry count written to audit MR.
 *
 * Returns SEL4_ERR_OK, SEL4_ERR_FORBIDDEN, or SEL4_ERR_NOT_FOUND.
 */
uint32_t handle_cap_audit(sel4_badge_t       badge,
                           const sel4_msg_t  *req,
                           sel4_msg_t        *rep,
                           void              *ctx)
{
    (void)ctx;

    /* ── Access control ──────────────────────────────────────────────────── */
    if (BADGE_CLIENT_ID(badge) != CONTROLLER_CLIENT_ID) {
        rep->opcode = SEL4_ERR_FORBIDDEN;
        rep->length = 0u;
        return SEL4_ERR_FORBIDDEN;
    }

    /* ── Extract pd_id filter ─────────────────────────────────────────────── */
    uint32_t pd_id = 0u;
    if (req->length >= 4u) {
        pd_id = (uint32_t)req->data[0]
              | ((uint32_t)req->data[1] << 8u)
              | ((uint32_t)req->data[2] << 16u)
              | ((uint32_t)req->data[3] << 24u);
    }

    /* ── Walk the accounting table ──────────────────────────────────────── */
    cap_audit_entry_t *mr_base = (cap_audit_entry_t *)(uintptr_t)g_audit_mr_vaddr;
    cap_audit_entry_t *mr_cur  = mr_base;
    uint32_t           count   = 0u;
    uint32_t           total   = cap_acct_count();

    for (uint32_t i = 0u; i < total && count < AUDIT_MAX_ENTRIES; i++) {
        const cap_acct_entry_t *e = cap_acct_get(i);
        if (!e) continue;

        /* Filter: pd_id == 0 means all PDs; otherwise match exactly */
        if (pd_id != 0u && e->pd_index != pd_id) continue;

        /*
         * Revocable determination:
         *   pd_index == 0  →  root-task initial caps (not revocable)
         *   pd_index > 0   →  derived caps (revocable)
         */
        uint8_t revocable = (e->pd_index > 0u) ? 1u : 0u;

        mr_cur = write_audit_entry(mr_cur, e, revocable);
        count++;
    }

    /* ── Write count into reply ──────────────────────────────────────────── */
    rep->opcode   = SEL4_ERR_OK;
    rep->length   = 4u;
    rep->data[0]  = (uint8_t)(count & 0xFFu);
    rep->data[1]  = (uint8_t)((count >> 8u)  & 0xFFu);
    rep->data[2]  = (uint8_t)((count >> 16u) & 0xFFu);
    rep->data[3]  = (uint8_t)((count >> 24u) & 0xFFu);

    return SEL4_ERR_OK;
}

/* ── OP_CAP_AUDIT_GUEST handler ──────────────────────────────────────────── */

/*
 * handle_cap_audit_guest — enumerate caps held by one vibeOS guest instance.
 *
 * Badge check: same as handle_cap_audit.
 *
 * req->data[0..3]: uint32_t vos_handle value.
 * rep->data[0..3]: uint32_t entry count.
 *
 * Returns SEL4_ERR_OK, SEL4_ERR_FORBIDDEN, or SEL4_ERR_NOT_FOUND.
 */
uint32_t handle_cap_audit_guest(sel4_badge_t       badge,
                                 const sel4_msg_t  *req,
                                 sel4_msg_t        *rep,
                                 void              *ctx)
{
    (void)ctx;

    /* ── Access control ──────────────────────────────────────────────────── */
    if (BADGE_CLIENT_ID(badge) != CONTROLLER_CLIENT_ID) {
        rep->opcode = SEL4_ERR_FORBIDDEN;
        rep->length = 0u;
        return SEL4_ERR_FORBIDDEN;
    }

    /* ── Extract vos_handle ──────────────────────────────────────────────── */
    uint32_t raw_handle = 0u;
    if (req->length >= 4u) {
        raw_handle = (uint32_t)req->data[0]
                   | ((uint32_t)req->data[1] << 8u)
                   | ((uint32_t)req->data[2] << 16u)
                   | ((uint32_t)req->data[3] << 24u);
    }

    vos_handle_t handle = (vos_handle_t)raw_handle;

    /* ── Look up the guest instance ──────────────────────────────────────── */
    const vos_instance_t *inst = vos_instance_get(handle);
    if (!inst) {
        rep->opcode = SEL4_ERR_NOT_FOUND;
        rep->length = 0u;
        return SEL4_ERR_NOT_FOUND;
    }

    /*
     * Walk the cap accounting table for entries owned by this guest.
     * Guest caps are recorded with pd_index == (uint32_t)handle.
     */
    cap_audit_entry_t *mr_base = (cap_audit_entry_t *)(uintptr_t)g_audit_mr_vaddr;
    cap_audit_entry_t *mr_cur  = mr_base;
    uint32_t           count   = 0u;
    uint32_t           total   = cap_acct_count();

    for (uint32_t i = 0u; i < total && count < AUDIT_MAX_ENTRIES; i++) {
        const cap_acct_entry_t *e = cap_acct_get(i);
        if (!e) continue;

        if (e->pd_index != (uint32_t)handle) continue;

        /* All guest caps are derived (revocable) */
        mr_cur = write_audit_entry(mr_cur, e, 1u);
        count++;
    }

    /* ── Write count into reply ──────────────────────────────────────────── */
    rep->opcode   = SEL4_ERR_OK;
    rep->length   = 4u;
    rep->data[0]  = (uint8_t)(count & 0xFFu);
    rep->data[1]  = (uint8_t)((count >> 8u)  & 0xFFu);
    rep->data[2]  = (uint8_t)((count >> 16u) & 0xFFu);
    rep->data[3]  = (uint8_t)((count >> 24u) & 0xFFu);

    return SEL4_ERR_OK;
}

/* ── Boot-time verification ──────────────────────────────────────────────── */

/*
 * cap_tree_verify_all_pds — log initial cap counts per PD.
 *
 * Called from main.c after all PDs are started to establish a regression
 * baseline.  Under AGENTOS_TEST_HOST this is a pure no-op; the host audit
 * buffer is available for inspection via cap_audit_test_get_entry.
 *
 * On target hardware this function would forward counts to LogSvc.
 */
void cap_tree_verify_all_pds(void)
{
    /*
     * Count caps per pd_index from the accounting table.
     * We track up to VERIFY_MAX_PDS distinct PD indices inline.
     */
#define VERIFY_MAX_PDS 64u

    uint32_t pd_counts[VERIFY_MAX_PDS];
    uint32_t pd_seen[VERIFY_MAX_PDS];
    uint32_t pd_n = 0u;

    for (uint32_t j = 0u; j < VERIFY_MAX_PDS; j++) {
        pd_counts[j] = 0u;
        pd_seen[j]   = (uint32_t)-1u;
    }

    uint32_t total = cap_acct_count();

    for (uint32_t i = 0u; i < total; i++) {
        const cap_acct_entry_t *e = cap_acct_get(i);
        if (!e) continue;

        uint32_t slot = VERIFY_MAX_PDS;
        for (uint32_t j = 0u; j < pd_n; j++) {
            if (pd_seen[j] == e->pd_index) { slot = j; break; }
        }
        if (slot == VERIFY_MAX_PDS) {
            if (pd_n < VERIFY_MAX_PDS) {
                slot = pd_n;
                pd_seen[pd_n]   = e->pd_index;
                pd_counts[pd_n] = 0u;
                pd_n++;
            } else {
                slot = VERIFY_MAX_PDS - 1u;
            }
        }
        pd_counts[slot]++;
    }

    /* Intentionally a no-op on both host and target:
     * results would be forwarded to LogSvc on hardware. */
    (void)pd_counts;
    (void)pd_n;
}

/* ── Test-only inspection helpers ────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

/*
 * cap_audit_test_get_entry — retrieve an entry from the host-side audit buffer.
 *
 * Used exclusively by test_cap_audit.c to inspect entries written by the
 * handlers.  Not part of the production ABI.
 */
const cap_audit_entry_t *cap_audit_test_get_entry(uint32_t index)
{
    if (index >= AUDIT_HOST_BUF_ENTRIES) return (const cap_audit_entry_t *)0;
    return &g_audit_host_buf[index];
}

/*
 * cap_audit_test_reset — zero the host-side audit buffer.
 *
 * Called at the start of each test that writes to the audit region.
 */
void cap_audit_test_reset(void)
{
    for (uint32_t i = 0u; i < AUDIT_HOST_BUF_ENTRIES; i++) {
        cap_audit_entry_t *e = &g_audit_host_buf[i];
        e->pd_id     = 0u;
        e->cslot     = 0u;
        e->cap_type  = 0u;
        e->revocable = 0u;
        e->_pad[0]   = 0u;
        e->_pad[1]   = 0u;
        e->_pad[2]   = 0u;
        e->name[0]   = '\0';
    }
}

#endif /* AGENTOS_TEST_HOST */
