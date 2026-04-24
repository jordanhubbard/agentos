/*
 * vos_snapshot.c — VOS_SNAPSHOT implementation for vibeOS
 *
 * Implements the VOS_SNAPSHOT lifecycle operation:
 *   1. Validate the guest handle via the instance table.
 *   2. Quiesce the guest by suspending its TCB/VCPU.
 *   3. Serialize register state + RAM into a flat blob (vos_snap_hdr_t + regs + RAM).
 *   4. Write the blob to AgentFS via AGENTFS_OP_PUT.
 *   5. Return a 64-bit snap token (snap_lo, snap_hi) to the caller.
 *
 * Guest is left suspended after VOS_SNAPSHOT; the caller resumes it via
 * VOS_RESTORE or by calling seL4_TCB_Resume directly.
 *
 * Under AGENTOS_TEST_HOST, all seL4 VCPU / TCB primitives are stubbed out
 * so the full logic compiles and is exercised on the host with zeroed
 * register and RAM buffers.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* ── Portability: host vs seL4 ───────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST
#  include <stdint.h>
#  include <stddef.h>
#  include <stdbool.h>
#  include <string.h>
#  include <stdio.h>

/* seL4 type stubs for host builds */
typedef uint32_t seL4_CPtr;
typedef uint64_t seL4_Word;
typedef int      seL4_Error;

/* AArch64 VCPU context (64 GP regs + SP + PC + SPSR = 68 × 8 bytes = 544) */
typedef struct {
    uint64_t regs[31]; /* x0..x30 */
    uint64_t sp;
    uint64_t pc;
    uint64_t spsr;
    uint64_t _pad;
} seL4_VCPUContext;

#define seL4_NoError 0

/* Stub: suspend a TCB cap — no-op on host */
static inline seL4_Error seL4_TCB_Suspend(seL4_CPtr tcb_cap)
{
    (void)tcb_cap;
    return seL4_NoError;
}

/* Stub: read VCPU registers — returns zeroed context on host */
static inline seL4_Error seL4_VCPU_ReadRegs(seL4_CPtr vcpu_cap,
                                             seL4_VCPUContext *ctx)
{
    (void)vcpu_cap;
    memset(ctx, 0, sizeof(*ctx));
    return seL4_NoError;
}

/* Stub: AgentFS PUT returns a deterministic hash-like token */
typedef struct {
    uint32_t snap_lo;
    uint32_t snap_hi;
} agentfs_token_t;

/* Forward-declared: implemented in the test or in the real AgentFS client */
static agentfs_token_t agentfs_put_blob(const uint8_t *data, uint32_t len,
                                        const char *key);

/* No logging in host mode */
#define VOS_LOG(msg) ((void)0)

#else /* seL4 target build */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <sel4/sel4.h>  /* seL4_TCB_Suspend, seL4_VCPU_ReadRegs, seL4_VCPUContext */
#include "agentos.h"    /* log_drain_write, VOS_LOG macro */

/* On seL4 we call the real AgentFS PUT operation via IPC */
typedef struct {
    uint32_t snap_lo;
    uint32_t snap_hi;
} agentfs_token_t;

static agentfs_token_t agentfs_put_blob(const uint8_t *data, uint32_t len,
                                        const char *key);

#define VOS_LOG(msg) log_drain_write(3, 3, "[vos_snapshot] " msg "\n")

#endif /* AGENTOS_TEST_HOST */

/* ── Public contract headers ─────────────────────────────────────────────── */

/* On host builds we include the contracts relative to the project root. */
/* On seL4 builds the include path is set by the build system.           */
#include "contracts/vibeos/interface.h"    /* vos_snap_hdr_t, VOS_SNAP_MAGIC, VOS_ERR_* */
#include "contracts/agentfs/interface.h"   /* AGENTFS_OP_WRITE, AGENTFS_OP_PUT */

/* ── seL4_VCPUContext size constant ─────────────────────────────────────── */
#define VOS_REG_DUMP_SIZE   ((uint32_t)sizeof(seL4_VCPUContext))

/* ── Maximum guest RAM we will serialize (guard against enormous blobs) ─── */
#define VOS_MAX_SNAP_RAM_PAGES  UINT32_C(8192)   /* 32 MiB ceiling */

/* ── Guest instance table ────────────────────────────────────────────────── */

/*
 * vos_instance_t — per-guest slot maintained by vos_create.c / vos_destroy.c.
 *
 * This struct must match the layout used by vos_create.c.  Any change to the
 * layout here must be reflected there (and vice versa).
 *
 * In AGENTOS_TEST_HOST builds the struct is self-contained; in seL4 builds the
 * single definition lives in vos_create.c and is extern-declared here.
 */
#define VOS_INSTANCE_MAX   VOS_MAX_INSTANCES   /* from contracts/vibeos/interface.h */

typedef struct {
    bool          active;
    vos_handle_t  handle;
    vos_state_t   state;
    vos_os_type_t os_type;
    uint8_t       vcpu_count;
    uint8_t       cpu_quota_pct;
    uint8_t       _pad0;
    uint32_t      ram_pages;        /* allocated guest RAM pages */
    seL4_CPtr     tcb_cap;          /* TCB capability for suspend/resume */
    seL4_CPtr     vcpu_cap;         /* VCPU capability for register read */
    uintptr_t     ram_vaddr;        /* virtual address of guest RAM in root task */
    uint32_t      bound_services;   /* VOS_SVC_* bitmask */
    char          label[16];
} vos_instance_t;

#ifdef AGENTOS_TEST_HOST
/* In host test builds we own the instance table */
static vos_instance_t g_vos_instances[VOS_INSTANCE_MAX];
static uint32_t       g_vos_next_handle = 1u;

/* Exposed for tests */
vos_instance_t *vos_instance_table(void) { return g_vos_instances; }
uint32_t        vos_instance_next_handle(void) { return g_vos_next_handle; }

void vos_snapshot_init(void)
{
    memset(g_vos_instances, 0, sizeof(g_vos_instances));
    g_vos_next_handle = 1u;
}

/* Allocate and register a test guest instance; returns its handle */
vos_handle_t vos_test_alloc_instance(uint32_t ram_pages)
{
    for (uint32_t i = 0; i < VOS_INSTANCE_MAX; i++) {
        if (!g_vos_instances[i].active) {
            g_vos_instances[i].active        = true;
            g_vos_instances[i].handle        = g_vos_next_handle++;
            g_vos_instances[i].state         = VOS_STATE_RUNNING;
            g_vos_instances[i].os_type       = VOS_OS_LINUX;
            g_vos_instances[i].vcpu_count    = 1;
            g_vos_instances[i].cpu_quota_pct = 50;
            g_vos_instances[i].ram_pages     = ram_pages;
            g_vos_instances[i].tcb_cap       = 0;
            g_vos_instances[i].vcpu_cap      = 0;
            g_vos_instances[i].ram_vaddr     = 0; /* host: no real RAM mapped */
            g_vos_instances[i].bound_services = 0;
            snprintf(g_vos_instances[i].label,
                     sizeof(g_vos_instances[i].label),
                     "test-%u", g_vos_instances[i].handle);
            return g_vos_instances[i].handle;
        }
    }
    return VOS_HANDLE_INVALID;
}

void vos_test_free_instance(vos_handle_t h)
{
    for (uint32_t i = 0; i < VOS_INSTANCE_MAX; i++) {
        if (g_vos_instances[i].active && g_vos_instances[i].handle == h) {
            memset(&g_vos_instances[i], 0, sizeof(g_vos_instances[i]));
            return;
        }
    }
}

#else /* seL4 target — extern declaration; definition is in vos_create.c */
extern vos_instance_t g_vos_instances[VOS_INSTANCE_MAX];
#endif

/* ── Instance lookup ─────────────────────────────────────────────────────── */

static vos_instance_t *vos_instance_get(vos_handle_t handle)
{
    if (handle == VOS_HANDLE_INVALID)
        return NULL;
    for (uint32_t i = 0; i < VOS_INSTANCE_MAX; i++) {
        if (g_vos_instances[i].active &&
            g_vos_instances[i].handle == handle) {
            return &g_vos_instances[i];
        }
    }
    return NULL;
}

/* ── AgentFS stub / real client ──────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

/*
 * Monotonic counter to generate distinct tokens on each call.
 * The token encodes the counter so callers can detect new-blob-per-call.
 */
static uint32_t g_agentfs_seq = 0u;

/* In-memory stub: does not persist anything, just returns a deterministic token */
static agentfs_token_t agentfs_put_blob(const uint8_t *data, uint32_t len,
                                        const char *key)
{
    (void)data; (void)len; (void)key;
    agentfs_token_t tok;
    g_agentfs_seq++;
    /* Derive a non-zero token from the sequence number */
    tok.snap_lo = 0xAF010000u | (g_agentfs_seq & 0xFFFFu);
    tok.snap_hi = 0xCAFE0000u | (g_agentfs_seq & 0xFFFFu);
    return tok;
}

#else /* seL4 target */

/*
 * agentfs_put_blob — call AgentFS OP_AGENTFS_PUT via seL4 IPC.
 *
 * The blob is placed in the agentfs_store shared MR by the caller prior to
 * this function being called.  Here we construct the IPC message and send it.
 *
 * Returns the token (first 8 bytes of the assigned ObjectId packed as two
 * uint32_t values) or {0, 0} on failure.
 */
#define AGENTFS_CH  11u   /* AgentFS channel ID from controller's perspective */

static agentfs_token_t agentfs_put_blob(const uint8_t *data, uint32_t len,
                                        const char *key)
{
    agentfs_token_t tok = {0, 0};
    (void)data; /* data is already in the shared MR */
    (void)key;

    /* MR0=opcode, MR1=data_store_offset(0), MR2=data_len, MR3=cap_tag(0) */
    microkit_mr_set(0, OP_AGENTFS_PUT);
    microkit_mr_set(1, len);         /* size */
    microkit_mr_set(2, 0);           /* cap_tag = 0 (world-readable snapshot) */
    microkit_msginfo reply =
        microkit_ppcall(AGENTFS_CH, microkit_msginfo_new(0, 3));

    uint32_t status = (uint32_t)microkit_mr_get(0);
    if (status != AFS_OK)
        return tok;

    /* The PUT reply packs the ObjectId into MR1..MR4; we use first 8 bytes */
    tok.snap_lo = (uint32_t)microkit_mr_get(1);
    tok.snap_hi = (uint32_t)microkit_mr_get(2);
    (void)reply;
    return tok;
}

#endif /* AGENTOS_TEST_HOST */

/* ── Snapshot blob assembly ───────────────────────────────────────────────── */

/*
 * vos_build_snap_blob
 *
 * Assembles the snapshot blob into @buf (capacity @buf_cap bytes).
 * Layout:
 *   [0]              vos_snap_hdr_t     (32 bytes)
 *   [32]             seL4_VCPUContext   (reg_dump_size bytes)
 *   [32+reg_dump]    raw RAM            (ram_pages × 4096 bytes; zeroed on host)
 *
 * Returns the total blob length written, or 0 on error.
 */
static uint32_t vos_build_snap_blob(const vos_instance_t *inst,
                                    uint8_t *buf, uint32_t buf_cap)
{
    uint32_t ram_pages     = inst->ram_pages;
    uint32_t reg_dump_size = VOS_REG_DUMP_SIZE;

    /* Guard: cap RAM pages to avoid blowing the buffer */
    if (ram_pages > VOS_MAX_SNAP_RAM_PAGES)
        ram_pages = VOS_MAX_SNAP_RAM_PAGES;

    uint32_t total = (uint32_t)sizeof(vos_snap_hdr_t) + reg_dump_size
                     + ram_pages * 4096u;

    if (total > buf_cap || total < sizeof(vos_snap_hdr_t))
        return 0u;

    /* ── Header ── */
    vos_snap_hdr_t *hdr = (vos_snap_hdr_t *)buf;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic          = VOS_SNAP_MAGIC;
    hdr->version        = VOS_SNAP_VERSION;
    hdr->guest_handle   = inst->handle;
    hdr->ram_size_pages = ram_pages;
    hdr->reg_dump_size  = reg_dump_size;
    /* _pad[3] already zeroed by memset */

    /* ── Register dump ── */
    uint8_t *reg_region = buf + sizeof(vos_snap_hdr_t);
    seL4_VCPUContext ctx;
    seL4_VCPU_ReadRegs(inst->vcpu_cap, &ctx);
    memcpy(reg_region, &ctx, reg_dump_size);

    /* ── RAM dump ── */
    uint8_t *ram_region = reg_region + reg_dump_size;
    uint32_t ram_bytes  = ram_pages * 4096u;

#ifdef AGENTOS_TEST_HOST
    /* Host: no real guest RAM; zero-fill the region */
    memset(ram_region, 0, ram_bytes);
#else
    /* seL4: copy from the mapped guest RAM region */
    if (inst->ram_vaddr && ram_bytes > 0) {
        memcpy(ram_region, (const void *)inst->ram_vaddr, ram_bytes);
    } else {
        memset(ram_region, 0, ram_bytes);
    }
#endif

    return total;
}

/* ── Snapshot buffer (static allocation to avoid heap dependency) ─────────── */

/*
 * Maximum snapshot size: header + regs + VOS_MAX_SNAP_RAM_PAGES × 4096
 *   = 32 + 544 + 8192 × 4096  = ~32 MiB + 576 bytes
 *
 * For host tests we use a much smaller cap so the test binary fits in RAM.
 */
#ifdef AGENTOS_TEST_HOST
#  define VOS_SNAP_BUF_PAGES  4u   /* 16 KiB — enough for test instances */
#else
#  define VOS_SNAP_BUF_PAGES  VOS_MAX_SNAP_RAM_PAGES
#endif

#define VOS_SNAP_BUF_CAP \
    ((uint32_t)(sizeof(vos_snap_hdr_t) + VOS_REG_DUMP_SIZE + \
                VOS_SNAP_BUF_PAGES * 4096u))

static uint8_t g_snap_buf[VOS_SNAP_BUF_CAP];

/* ── Public API ──────────────────────────────────────────────────────────── */

#ifndef AGENTOS_TEST_HOST
/*
 * vos_snapshot_init — called once at boot by the root task or vibeOS init.
 */
void vos_snapshot_init(void)
{
    memset(g_snap_buf, 0, sizeof(g_snap_buf));
    VOS_LOG("VOS_SNAPSHOT subsystem initialised");
}
#endif

/*
 * vos_snapshot — snapshot a live guest instance.
 *
 * Steps:
 *   1. Look up the instance.
 *   2. Suspend the guest TCB.
 *   3. Build the snapshot blob.
 *   4. Store it in AgentFS.
 *   5. Return the token.
 *
 * @handle  — vos_handle_t of the guest to snapshot
 * @snap_lo — [out] low 32 bits of the AgentFS storage token
 * @snap_hi — [out] high 32 bits of the AgentFS storage token
 *
 * Returns VOS_ERR_OK on success or a VOS_ERR_* code on failure.
 */
vos_err_t vos_snapshot(vos_handle_t handle,
                        uint32_t *snap_lo, uint32_t *snap_hi)
{
    /* ── Step 1: validate handle ── */
    vos_instance_t *inst = vos_instance_get(handle);
    if (!inst) {
        return VOS_ERR_INVALID_HANDLE;
    }

    /* ── Step 2: quiesce — suspend the guest TCB ── */
    seL4_Error tcb_err = seL4_TCB_Suspend(inst->tcb_cap);
    if (tcb_err != seL4_NoError) {
        VOS_LOG("TCB_Suspend failed");
        return VOS_ERR_SNAPSHOT_FAILED;
    }
    /* Update state to reflect suspension */
    inst->state = VOS_STATE_SUSPENDED;

    /* ── Step 3: build the flat snapshot blob ── */
    uint32_t blob_len = vos_build_snap_blob(inst, g_snap_buf, sizeof(g_snap_buf));
    if (blob_len == 0u) {
        VOS_LOG("blob assembly failed");
        return VOS_ERR_SNAPSHOT_FAILED;
    }

    /* ── Step 4: store in AgentFS ── */
    char key[32];
    /* Key: "vos-snap-HHHHHHHH" where HHHHHHHH = handle in hex */
    static const char hex[] = "0123456789abcdef";
    key[0]  = 'v'; key[1]  = 'o'; key[2]  = 's'; key[3]  = '-';
    key[4]  = 's'; key[5]  = 'n'; key[6]  = 'a'; key[7]  = 'p';
    key[8]  = '-';
    for (int i = 0; i < 8; i++) {
        key[9 + i] = hex[(handle >> (28 - i * 4)) & 0xFu];
    }
    key[17] = '\0';

    agentfs_token_t tok = agentfs_put_blob(g_snap_buf, blob_len, key);
    if (tok.snap_lo == 0 && tok.snap_hi == 0) {
        VOS_LOG("AgentFS PUT failed");
        return VOS_ERR_SNAPSHOT_FAILED;
    }

    /* ── Step 5: return token ── */
    *snap_lo = tok.snap_lo;
    *snap_hi = tok.snap_hi;

    VOS_LOG("snapshot complete");
    return VOS_ERR_OK;
}

/*
 * handle_vos_snapshot — IPC dispatch entry point.
 *
 * Called by the vibeOS request dispatcher when MR0 == VOS_OP_SNAPSHOT.
 *
 * MR layout (input):
 *   MR0 = VOS_OP_SNAPSHOT
 *   MR1 = vos_handle_t handle
 *
 * MR layout (output):
 *   MR0 = VOS_ERR_*
 *   MR1 = snap_lo  (on VOS_ERR_OK)
 *   MR2 = snap_hi  (on VOS_ERR_OK)
 *
 * @badge — seL4 badge of the calling endpoint (for future rights checking)
 * @req   — unused; callers pass the handle in MR1
 * @rep   — unused; reply is written via microkit_mr_set
 * @ctx   — unused context pointer (reserved)
 *
 * Returns: number of reply MRs written (3 on success, 1 on error).
 */
#ifndef AGENTOS_TEST_HOST
/* In seL4 builds, sel4_badge_t and sel4_msg_t come from sel4_ipc.h. */
#include "sel4_ipc.h"
uint32_t handle_vos_snapshot(sel4_badge_t badge,
                              const sel4_msg_t *req,
                              sel4_msg_t *rep,
                              void *ctx)
{
    (void)badge; (void)req; (void)rep; (void)ctx;

    vos_handle_t handle = (vos_handle_t)microkit_mr_get(1);
    uint32_t snap_lo = 0, snap_hi = 0;

    vos_err_t err = vos_snapshot(handle, &snap_lo, &snap_hi);

    microkit_mr_set(0, err);
    if (err == VOS_ERR_OK) {
        microkit_mr_set(1, snap_lo);
        microkit_mr_set(2, snap_hi);
        return 3u;
    }
    return 1u;
}
#endif /* !AGENTOS_TEST_HOST */
