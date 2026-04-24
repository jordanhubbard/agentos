/*
 * vos_restore.c — VOS_OP_RESTORE implementation.
 *
 * Replays a previously captured VOS_SNAPSHOT blob back into a fresh guest
 * OS instance.  The (snap_lo, snap_hi) token returned by VOS_OP_SNAPSHOT
 * identifies the blob in AgentFS.
 *
 * Restore sequence:
 *   1. Validate the (snap_lo, snap_hi) token — reject zero tokens.
 *   2. Fetch the blob from AgentFS (or the host-stub store).
 *   3. Validate the vos_snap_hdr_t magic and version.
 *   4. Allocate a fresh guest instance slot.
 *   5. Restore vCPU register state from the blob.
 *   6. Copy guest RAM from the blob back into the instance's RAM mapping.
 *   7. Resume the guest (mark it RUNNING).
 *   8. Return the new guest_handle in the IPC reply.
 *
 * AGENTOS_TEST_HOST:
 *   All seL4 primitives are stubbed.  The blob is fetched from the in-memory
 *   store written by vos_snapshot.c via vos_test_snap_store_get().  Instance
 *   allocation uses vos_test_alloc_instance() from the same instance table in
 *   vos_snapshot.c.
 *
 * No microkit.h is included — only sel4_boot.h/sel4_ipc.h types on real
 * hardware and stdlib headers under AGENTOS_TEST_HOST.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "vos_snap_store.h"

/* Pull in the public API contract for opcodes, error codes, and structs */
#include "contracts/vibeos/interface.h"

#ifndef AGENTOS_TEST_HOST
#include "sel4_boot.h"
#endif

/* ── Forward-declared types from vos_snapshot.c ─────────────────────────── */

/*
 * vos_instance_t is defined in vos_snapshot.c.  Rather than placing it in a
 * separate header (which would pull in the entire instance-table API), we
 * forward-declare the opaque type here and access it only via the two helpers
 * that are publicly visible: vos_test_alloc_instance() and vos_instance_get().
 *
 * On real seL4, a proper header would be used.  For the test build these
 * declarations are sufficient.
 */
typedef struct vos_instance vos_instance_t;

/*
 * Functions implemented in vos_snapshot.c, accessible here for the shared
 * instance table.
 */
extern vos_instance_t *vos_test_alloc_instance(void);
extern vos_instance_t *vos_instance_get(uint32_t handle);

/*
 * Accessor helpers — reach into the vos_instance_t fields.
 *
 * Because vos_restore.c is compiled alongside vos_snapshot.c in the test
 * build, we can use a forward declaration trick: the real struct definition
 * is in vos_snapshot.c and the linker resolves everything at link time.
 *
 * For clarity we expose only the fields we need via accessor macros that
 * cast through void* (safe because the layout is the same binary).  In a
 * larger codebase these would be proper accessor functions in a header.
 *
 * Layout offsets for vos_instance_t (must match vos_snapshot.c):
 *   handle        : uint32_t   @ 0
 *   state         : uint8_t    @ 4   (vos_state_t)
 *   os_type       : uint8_t    @ 5   (vos_os_type_t)
 *   vcpu_count    : uint8_t    @ 6
 *   _pad          : uint8_t[1] @ 7
 *   tcb_cap       : uint32_t   @ 8
 *   vcpu_cap      : uint32_t   @ 12
 *   memory_pages  : uint32_t   @ 16
 *   ram_vaddr     : uintptr_t  @ 20  (size depends on pointer width)
 *   test_regs[32] : uint32_t[32] @ 20+sizeof(uintptr_t)
 *   test_pc       : uint64_t
 *   active        : bool
 *   label[16]     : char[16]
 *
 * Rather than computing all those offsets manually we use the full struct
 * definition here — it is identical to the one in vos_snapshot.c.  Both
 * translation units see the same layout; the linker sees only one copy of the
 * data since the actual storage is in vos_snapshot.c.
 */

/* Replicate the struct so this TU can access fields without a shared header */
struct vos_instance {
    uint32_t    handle;
    vos_state_t state;
    vos_os_type_t os_type;
    uint8_t     vcpu_count;
    uint8_t     _pad[1];
    uint32_t    tcb_cap;
    uint32_t    vcpu_cap;
    uint32_t    memory_pages;
    uintptr_t   ram_vaddr;
    uint32_t    test_regs[32];
    uint64_t    test_pc;
    bool        active;
    char        label[16];
};

/* ── seL4 primitive stubs ────────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

static inline void seL4_TCB_Resume(uint32_t tcb_cap) { (void)tcb_cap; }

static inline void seL4_VCPU_WriteRegs(uint32_t vcpu_cap,
                                        const uint32_t regs[32], uint64_t pc)
{
    (void)vcpu_cap; (void)regs; (void)pc;
}

#endif /* AGENTOS_TEST_HOST */

/* ── AgentFS blob retrieval ───────────────────────────────────────────────── */

/*
 * VOS_RESTORE_BUF_BYTES — size of the static receive buffer.
 *
 * Must be large enough for the largest expected blob:
 *   header (32) + regs (128+8) + RAM stub (64 KiB)
 */
#define VOS_RESTORE_BUF_BYTES \
    (sizeof(vos_snap_hdr_t) + 32u * sizeof(uint32_t) + sizeof(uint64_t) + \
     64u * 1024u)

/*
 * agentfs_get_blob — fetch a snapshot blob identified by (snap_lo, snap_hi).
 *
 * On real seL4 hardware this would issue an AGENTFS_OP_GET IPC.
 * Under AGENTOS_TEST_HOST it delegates to vos_test_snap_store_get().
 *
 * Returns number of bytes written on success, 0 if not found, negative on
 * unexpected error.
 */
static int agentfs_get_blob(uint32_t snap_lo, uint32_t snap_hi,
                             void *buf, uint32_t buf_size)
{
#ifdef AGENTOS_TEST_HOST
    return vos_test_snap_store_get(snap_lo, snap_hi, buf, buf_size);
#else
    /*
     * Real implementation:
     *   - Encode snap_lo/snap_hi into an agentfs_object_id_t.
     *   - Issue AGENTFS_OP_GET IPC to the AgentFS PD.
     *   - Copy the returned bytes from the AgentFS shared-memory window.
     */
    (void)snap_lo; (void)snap_hi; (void)buf; (void)buf_size;
    return -1;  /* not implemented on non-test path */
#endif
}

/* ── Restore entry point ─────────────────────────────────────────────────── */

/*
 * vos_restore — restore a guest OS instance from a snapshot blob.
 *
 * Called by the root-task IPC dispatcher when it receives VOS_OP_RESTORE.
 * On success returns VOS_ERR_OK and writes the new guest_handle to
 * *out_guest_handle.
 */
uint32_t vos_restore(uint32_t snap_lo, uint32_t snap_hi,
                      uint32_t *out_guest_handle)
{
    /* Step 1: Validate token — both zero → invalid */
    if (snap_lo == 0 && snap_hi == 0)
        return VOS_ERR_INVALID_HANDLE;

    /* Step 2: Fetch blob from AgentFS */
    static uint8_t s_restore_buf[VOS_RESTORE_BUF_BYTES];

    int fetched = agentfs_get_blob(snap_lo, snap_hi,
                                    s_restore_buf, sizeof(s_restore_buf));
    if (fetched <= 0)
        return VOS_ERR_SNAP_NOT_FOUND;

    /* Step 3: Validate snapshot header */
    if ((uint32_t)fetched < sizeof(vos_snap_hdr_t))
        return VOS_ERR_SNAP_CORRUPT;

    const vos_snap_hdr_t *hdr = (const vos_snap_hdr_t *)s_restore_buf;

    if (hdr->magic != VOS_SNAP_MAGIC)
        return VOS_ERR_SNAP_CORRUPT;

    if (hdr->version != VOS_SNAP_VERSION)
        return VOS_ERR_SNAP_CORRUPT;

    /* Verify total blob is at least as large as declared */
    uint32_t expected_size = (uint32_t)sizeof(vos_snap_hdr_t) +
                             32u * (uint32_t)sizeof(uint32_t) +
                             (uint32_t)sizeof(uint64_t) +
                             (uint32_t)hdr->ram_bytes;

    if ((uint32_t)fetched < expected_size)
        return VOS_ERR_SNAP_CORRUPT;

    /* Step 4: Allocate a fresh guest instance */
    vos_instance_t *inst = vos_test_alloc_instance();
    if (!inst)
        return VOS_ERR_OUT_OF_MEMORY;

    /* Populate basic instance metadata from the snapshot header */
    inst->os_type    = (vos_os_type_t)hdr->os_type;
    inst->vcpu_count = (uint8_t)(hdr->vcpu_count ? hdr->vcpu_count : 1u);

    /* Step 5: Restore register state */
    const uint8_t *p = s_restore_buf + sizeof(vos_snap_hdr_t);

    const uint32_t *snap_regs = (const uint32_t *)p;
    p += 32u * sizeof(uint32_t);

    uint64_t snap_pc;
    memcpy(&snap_pc, p, sizeof(uint64_t));
    p += sizeof(uint64_t);

#ifdef AGENTOS_TEST_HOST
    /* Store register dump back into the test instance for verification */
    for (uint32_t i = 0; i < 32; i++)
        inst->test_regs[i] = snap_regs[i];
    inst->test_pc = snap_pc;
#else
    seL4_VCPU_WriteRegs(inst->vcpu_cap, snap_regs, snap_pc);
#endif

    /* Step 6: Restore guest RAM */
#ifdef AGENTOS_TEST_HOST
    /*
     * On host we do not have a real guest RAM mapping.  The test verifies
     * the round-trip by comparing test_regs and test_pc, which are set above.
     * If a test needs RAM verification it can read back from the blob directly
     * via vos_test_snap_store_get().
     */
    (void)p;  /* suppress unused-variable warning */
#else
    if (inst->ram_vaddr && hdr->ram_bytes > 0) {
        memcpy((void *)inst->ram_vaddr, p, (size_t)hdr->ram_bytes);
    }
#endif

    /* Step 7: Resume the guest */
    inst->state = VOS_STATE_RUNNING;

#ifndef AGENTOS_TEST_HOST
    seL4_TCB_Resume(inst->tcb_cap);
#endif

    /* Step 8: Return the new handle */
    *out_guest_handle = inst->handle;
    return VOS_ERR_OK;
}

/* ── IPC dispatch entry point ────────────────────────────────────────────── */

/*
 * handle_vos_restore — decode the VOS_OP_RESTORE IPC request and call
 * vos_restore().
 *
 * This function matches the handler signature used by the root-task IPC
 * dispatcher in ipc_harness.c.
 *
 *   req->data[0] = VOS_OP_RESTORE  (opcode — already checked by dispatcher)
 *   req->data[1] = snap_lo
 *   req->data[2] = snap_hi
 *   req->data[3] = _pad
 *
 * On success:
 *   rep->data[0] = VOS_ERR_OK
 *   rep->data[1] = guest_handle
 *
 * On failure:
 *   rep->data[0] = VOS_ERR_*
 */

/* Minimal sel4_msg_t shim so this file compiles without the full IPC header */
#ifndef SEL4_MSG_T_DEFINED
#define SEL4_MSG_T_DEFINED
typedef uint32_t sel4_badge_t;
typedef struct { uint32_t data[8]; } sel4_msg_t;
#define data_rd32(msg, idx)         ((msg)->data[(idx)])
#define data_wr32(msg, idx, val)    ((msg)->data[(idx)] = (uint32_t)(val))
#endif

uint32_t handle_vos_restore(sel4_badge_t badge,
                              const sel4_msg_t *req,
                              sel4_msg_t *rep,
                              void *ctx)
{
    (void)badge;
    (void)ctx;

    uint32_t snap_lo = data_rd32(req, 1);
    uint32_t snap_hi = data_rd32(req, 2);

    uint32_t new_handle = 0;
    uint32_t status = vos_restore(snap_lo, snap_hi, &new_handle);

    data_wr32(rep, 0, status);
    data_wr32(rep, 1, new_handle);
    data_wr32(rep, 2, 0);
    data_wr32(rep, 3, 0);

    return status;
}
