/*
 * vos_snapshot.c — VOS_OP_SNAPSHOT implementation.
 *
 * Captures a point-in-time snapshot of a guest OS instance and persists the
 * blob to AgentFS.  Returns a (snap_lo, snap_hi) storage token to the caller.
 *
 * On real seL4 hardware:
 *   1. Suspend the guest vCPU via seL4_TCB_Suspend.
 *   2. Read vCPU register state via seL4_VCPU_ReadRegs (AArch64) or
 *      seL4_TCB_ReadRegisters (x86).
 *   3. Walk guest RAM frames and copy to the snapshot buffer.
 *   4. Build a vos_snap_hdr_t + register dump + RAM blob.
 *   5. Call agentfs_put_blob() to persist the blob.
 *   6. Resume the guest via seL4_TCB_Resume.
 *   7. Return (snap_lo, snap_hi) in the IPC reply.
 *
 * Under AGENTOS_TEST_HOST:
 *   seL4 primitives are stubbed.  The blob is stored in a static in-memory
 *   table via vos_test_snap_store_put().  vos_restore.c retrieves blobs from
 *   the same table via vos_test_snap_store_get().
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

/* ── Internal instance table ─────────────────────────────────────────────── */

/*
 * vos_instance_t — per-guest runtime state tracked by the root task.
 *
 * On real seL4 hardware these fields hold seL4 capability indices.
 * Under AGENTOS_TEST_HOST they hold stub values used by tests.
 */
typedef struct {
    uint32_t    handle;         /* opaque handle (slot_index + 1)            */
    vos_state_t state;          /* current lifecycle state                   */
    vos_os_type_t os_type;      /* guest OS type                             */
    uint8_t     vcpu_count;
    uint8_t     _pad[1];
    uint32_t    tcb_cap;        /* seL4 TCB capability index (stub: 0)       */
    uint32_t    vcpu_cap;       /* seL4 VCPU capability index (stub: 0)      */
    uint32_t    memory_pages;   /* guest RAM in 4 KiB pages                  */
    uintptr_t   ram_vaddr;      /* virtual address of guest RAM mapping      */

    /*
     * Test-only register dump: 32 GP regs (x0-x30 + SP) + PC.
     * On real seL4, these would be read via seL4_VCPU_ReadRegs.
     */
    uint32_t    test_regs[32];
    uint64_t    test_pc;

    bool        active;         /* slot is in use                            */
    char        label[16];
} vos_instance_t;

#define VOS_MAX_SLOTS  4u

static vos_instance_t g_instances[VOS_MAX_SLOTS];
static uint32_t       g_next_handle = 1u;

/* ── Instance table helpers ─────────────────────────────────────────────── */

/*
 * vos_instance_get — look up a live instance by handle.
 * Returns NULL if the handle is not found or the slot is not active.
 */
vos_instance_t *vos_instance_get(uint32_t handle)
{
    for (uint32_t i = 0; i < VOS_MAX_SLOTS; i++) {
        if (g_instances[i].active && g_instances[i].handle == handle)
            return &g_instances[i];
    }
    return NULL;
}

/*
 * vos_test_alloc_instance — allocate a new blank instance (AGENTOS_TEST_HOST
 * and real seL4 create path both use this).
 * Returns NULL if no free slot is available.
 */
vos_instance_t *vos_test_alloc_instance(void)
{
    for (uint32_t i = 0; i < VOS_MAX_SLOTS; i++) {
        if (!g_instances[i].active) {
            vos_instance_t *inst = &g_instances[i];
            memset(inst, 0, sizeof(*inst));
            inst->handle = g_next_handle++;
            inst->state  = VOS_STATE_CREATING;
            inst->active = true;
            return inst;
        }
    }
    return NULL;
}

/*
 * vos_test_free_instance — release a slot back to the pool.
 */
void vos_test_free_instance(vos_instance_t *inst)
{
    if (inst) {
        memset(inst, 0, sizeof(*inst));
    }
}

/*
 * vos_test_instance_table_reset — clear ALL instance slots and reset the
 * handle counter.  Call this at the start of each test to guarantee a clean
 * slate regardless of how many handles were allocated by previous tests.
 */
void vos_test_instance_table_reset(void)
{
    memset(g_instances, 0, sizeof(g_instances));
    g_next_handle = 1u;
}

/* ── seL4 primitive stubs ────────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

/* Stub: suspend the guest TCB (no-op on host) */
static inline void seL4_TCB_Suspend(uint32_t tcb_cap)  { (void)tcb_cap; }

/* Stub: resume the guest TCB (on host, mark instance RUNNING) */
static inline void seL4_TCB_Resume(uint32_t tcb_cap)   { (void)tcb_cap; }

/* Stub: read vCPU registers — returns zeros */
static inline void seL4_VCPU_ReadRegs(uint32_t vcpu_cap,
                                       uint32_t regs[32], uint64_t *pc)
{
    (void)vcpu_cap;
    for (uint32_t i = 0; i < 32; i++) regs[i] = 0;
    *pc = 0;
}

/* Stub: write vCPU registers — no-op */
static inline void seL4_VCPU_WriteRegs(uint32_t vcpu_cap,
                                        const uint32_t regs[32], uint64_t pc)
{
    (void)vcpu_cap; (void)regs; (void)pc;
}

#endif /* AGENTOS_TEST_HOST */

/* ── AgentFS blob persistence ─────────────────────────────────────────────── */

/*
 * agentfs_put_blob — store a snapshot blob in AgentFS and return a token.
 *
 * On real seL4 hardware this would make an IPC call to the AgentFS PD.
 * Under AGENTOS_TEST_HOST it delegates to vos_test_snap_store_put().
 *
 * Returns VOS_ERR_OK on success, VOS_ERR_SNAPSHOT_FAILED on failure.
 */
static uint32_t agentfs_put_blob(const void *buf, uint32_t size,
                                  uint32_t *out_snap_lo, uint32_t *out_snap_hi)
{
#ifdef AGENTOS_TEST_HOST
    int rc = vos_test_snap_store_put(buf, size, out_snap_lo, out_snap_hi);
    return (rc == 0) ? VOS_ERR_OK : VOS_ERR_SNAPSHOT_FAILED;
#else
    /*
     * Real implementation:
     *   - Write buf into the AgentFS shared-memory window.
     *   - Issue AGENTFS_OP_PUT IPC to the AgentFS PD.
     *   - Extract the returned ObjectId lo/hi from MR1/MR2.
     */
    (void)buf; (void)size; (void)out_snap_lo; (void)out_snap_hi;
    return VOS_ERR_NOT_SUPPORTED;
#endif
}

/* ── Snapshot blob builder ────────────────────────────────────────────────── */

/*
 * VOS_SNAP_RAM_STUB_BYTES — amount of guest RAM we copy in stub/test mode.
 *
 * A real implementation would copy all memory_pages × 4096 bytes.
 * In the test stub we use a fixed 64 KiB so tests run fast and the static
 * blob store does not need to be enormous.
 */
#define VOS_SNAP_RAM_STUB_BYTES  (64u * 1024u)

/*
 * vos_build_snap_blob — serialise a guest instance into a flat byte buffer.
 *
 * Layout:
 *   [0]                  vos_snap_hdr_t  (32 bytes)
 *   [32]                 regs[0..31]     (32 × 4 = 128 bytes)
 *   [160]                pc              (8 bytes)
 *   [168]                guest RAM       (hdr.ram_bytes bytes)
 *
 * Returns total bytes written, or 0 on error (buf too small).
 */
static uint32_t vos_build_snap_blob(vos_instance_t *inst,
                                     uint8_t *buf, uint32_t buf_size,
                                     uint64_t timestamp_ns)
{
    uint32_t ram_bytes;

#ifdef AGENTOS_TEST_HOST
    ram_bytes = VOS_SNAP_RAM_STUB_BYTES;
#else
    ram_bytes = inst->memory_pages * 4096u;
#endif

    uint32_t total = sizeof(vos_snap_hdr_t) + 32u * sizeof(uint32_t) +
                     sizeof(uint64_t) + ram_bytes;

    if (buf_size < total)
        return 0;

    /* Header */
    vos_snap_hdr_t *hdr = (vos_snap_hdr_t *)buf;
    hdr->magic        = VOS_SNAP_MAGIC;
    hdr->version      = VOS_SNAP_VERSION;
    hdr->guest_handle = inst->handle;
    hdr->os_type      = (uint8_t)inst->os_type;
    hdr->_pad[0]      = 0;
    hdr->_pad[1]      = 0;
    hdr->_pad[2]      = 0;
    hdr->vcpu_count   = inst->vcpu_count ? inst->vcpu_count : 1u;
    hdr->ram_bytes    = ram_bytes;
    hdr->timestamp_ns = timestamp_ns;

    uint8_t *p = buf + sizeof(vos_snap_hdr_t);

    /* vCPU register state */
    uint32_t regs[32];
    uint64_t pc = 0;

#ifdef AGENTOS_TEST_HOST
    /* Use the test register state stored in the instance */
    for (uint32_t i = 0; i < 32; i++) regs[i] = inst->test_regs[i];
    pc = inst->test_pc;
#else
    seL4_VCPU_ReadRegs(inst->vcpu_cap, regs, &pc);
#endif

    memcpy(p, regs, 32u * sizeof(uint32_t));
    p += 32u * sizeof(uint32_t);
    memcpy(p, &pc, sizeof(uint64_t));
    p += sizeof(uint64_t);

    /* Guest RAM */
#ifdef AGENTOS_TEST_HOST
    /*
     * Stub: fill RAM region with a deterministic pattern based on the handle
     * so that round-trip tests can verify the data was preserved.
     */
    for (uint32_t i = 0; i < ram_bytes; i++) {
        p[i] = (uint8_t)((uint8_t)inst->handle ^ (uint8_t)i);
    }
#else
    if (inst->ram_vaddr) {
        memcpy(p, (const void *)inst->ram_vaddr, ram_bytes);
    } else {
        memset(p, 0, ram_bytes);
    }
#endif

    return total;
}

/* ── Public snapshot entry point ─────────────────────────────────────────── */

/*
 * vos_snapshot — capture a snapshot of the named guest instance.
 *
 * Called by the root-task IPC dispatcher when it receives VOS_OP_SNAPSHOT.
 *
 * On success returns VOS_ERR_OK and writes (snap_lo, snap_hi) to the
 * out-parameters.
 */
uint32_t vos_snapshot(uint32_t guest_handle,
                       uint32_t *out_snap_lo, uint32_t *out_snap_hi)
{
    vos_instance_t *inst = vos_instance_get(guest_handle);
    if (!inst)
        return VOS_ERR_INVALID_HANDLE;

    /* Suspend guest for the duration of the capture */
#ifndef AGENTOS_TEST_HOST
    seL4_TCB_Suspend(inst->tcb_cap);
#endif

    /* Serialise to a static scratch buffer */
    static uint8_t s_snap_buf[sizeof(vos_snap_hdr_t) +
                               32u * sizeof(uint32_t) +
                               sizeof(uint64_t) +
                               VOS_SNAP_RAM_STUB_BYTES];

    uint32_t blob_size = vos_build_snap_blob(inst, s_snap_buf, sizeof(s_snap_buf), 0u);
    if (blob_size == 0) {
#ifndef AGENTOS_TEST_HOST
        seL4_TCB_Resume(inst->tcb_cap);
#endif
        return VOS_ERR_SNAPSHOT_FAILED;
    }

    uint32_t err = agentfs_put_blob(s_snap_buf, blob_size, out_snap_lo, out_snap_hi);

    /* Resume guest */
#ifndef AGENTOS_TEST_HOST
    seL4_TCB_Resume(inst->tcb_cap);
#endif

    return err;
}

/* ── AGENTOS_TEST_HOST shared snap store ─────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

/*
 * The static blob store is defined here (in vos_snapshot.c) and accessed by
 * vos_restore.c via the declarations in vos_snap_store.h.
 */

typedef struct {
    bool     used;
    uint32_t size;
    uint8_t  data[VOS_SNAP_STORE_MAX_BLOB_BYTES];
} snap_store_entry_t;

static snap_store_entry_t s_snap_store[VOS_SNAP_STORE_MAX];

int vos_test_snap_store_put(const void *buf, uint32_t size,
                             uint32_t *out_snap_lo, uint32_t *out_snap_hi)
{
    if (!buf || size == 0 || size > VOS_SNAP_STORE_MAX_BLOB_BYTES)
        return -1;

    for (uint32_t i = 0; i < VOS_SNAP_STORE_MAX; i++) {
        if (!s_snap_store[i].used) {
            s_snap_store[i].used = true;
            s_snap_store[i].size = size;
            memcpy(s_snap_store[i].data, buf, size);
            *out_snap_lo = i + 1u;
            *out_snap_hi = ~(i + 1u);
            return 0;
        }
    }
    return -1;  /* store full */
}

int vos_test_snap_store_get(uint32_t snap_lo, uint32_t snap_hi,
                             void *buf, uint32_t buf_size)
{
    if (snap_lo == 0)
        return -1;

    uint32_t idx = snap_lo - 1u;
    if (idx >= VOS_SNAP_STORE_MAX)
        return -1;

    /* Verify complementary token */
    if (snap_hi != ~snap_lo)
        return -1;

    if (!s_snap_store[idx].used)
        return -1;

    uint32_t copy_bytes = s_snap_store[idx].size;
    if (copy_bytes > buf_size)
        copy_bytes = buf_size;

    memcpy(buf, s_snap_store[idx].data, copy_bytes);
    return (int)copy_bytes;
}

void vos_test_snap_store_reset(void)
{
    memset(s_snap_store, 0, sizeof(s_snap_store));
}

#endif /* AGENTOS_TEST_HOST */
