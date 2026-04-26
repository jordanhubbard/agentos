/*
 * agentOS GPU Scheduler Protection Domain
 *
 * Manages 4 static GPU compute slots for CUDA PTX offload.
 * Agents submit CUDA kernels (embedded as PTX in WASM custom section
 * "agentos.cuda") via VibeEngine. On approval, VibeEngine notifies
 * gpu_scheduler with OP_GPU_SUBMIT.
 *
 * On real hardware (Sparky GB10 Blackwell), the stub here would call
 * nvrtc to JIT-compile PTX and bind to a CUDA context.  In simulation
 * (QEMU RISC-V / AArch64) the slots are purely bookkeeping.
 *
 * Channel assignments:
 *   CH_VIBE = 0  — VibeEngine notifies us (GPU_SUBMIT / GPU_COMPLETE)
 *   CH_CTRL = 1  — (reserved) controller can query slot status
 *
 * IPC op codes (MR0):
 *   OP_GPU_SUBMIT   = 0x50  — claim a slot for a new CUDA kernel
 *   OP_GPU_COMPLETE = 0x51  — release a slot when kernel finishes
 *   OP_GPU_STATUS   = 0x52  — return busy bitmask of all 4 slots
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "agentos.h"
#include "sel4_server.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Channel IDs ────────────────────────────────────────────────────── */
#define CH_VIBE  0
#define CH_CTRL  1

/* ── Op codes ───────────────────────────────────────────────────────── */
#define OP_GPU_SUBMIT    0x50
#define OP_GPU_COMPLETE  0x51
#define OP_GPU_STATUS    0x52

/* ── Result codes ───────────────────────────────────────────────────── */
#define GPU_OK           0
#define GPU_ERR_FULL     1   /* all slots busy */
#define GPU_ERR_BADSLOT  2   /* invalid slot index */
#define GPU_ERR_IDLE     3   /* slot is not busy */

/* ── GPU slot table ─────────────────────────────────────────────────── */
#define NUM_GPU_SLOTS    4

typedef struct {
    bool     busy;
    uint32_t proposal_slot;   /* which vibe_engine proposal is loaded */
    uint32_t ptx_offset;      /* PTX offset within staging region */
    uint32_t ptx_len;         /* PTX payload length */
    uint32_t module_id;       /* monotonic JIT module id */
} gpu_slot_t;

static gpu_slot_t gpu_slots[NUM_GPU_SLOTS];
static uint32_t   next_module_id = 1;
static uint32_t   total_submitted = 0;
static uint32_t   total_completed = 0;

/* ── Helpers ────────────────────────────────────────────────────────── */

static int find_free_slot(void) {
    for (int i = 0; i < NUM_GPU_SLOTS; i++) {
        if (!gpu_slots[i].busy) return i;
    }
    return -1;
}

static uint32_t busy_bitmask(void) {
    uint32_t mask = 0;
    for (int i = 0; i < NUM_GPU_SLOTS; i++) {
        if (gpu_slots[i].busy) mask |= (1u << i);
    }
    return mask;
}

/* ── IPC handlers ───────────────────────────────────────────────────── */

static uint32_t handle_submit(void) {
    IPC_STUB_LOCALS
    uint32_t prop_slot  = (uint32_t)msg_u32(req, 4);
    uint32_t ptx_offset = (uint32_t)msg_u32(req, 8);
    uint32_t ptx_len    = (uint32_t)msg_u32(req, 12);

    int slot = find_free_slot();
    if (slot < 0) {
        log_drain_write(15, 15, "[gpu_scheduler] REJECT: all 4 GPU slots busy\n");
        rep_u32(rep, 0, GPU_ERR_FULL);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    gpu_slots[slot].busy         = true;
    gpu_slots[slot].proposal_slot = prop_slot;
    gpu_slots[slot].ptx_offset   = ptx_offset;
    gpu_slots[slot].ptx_len      = ptx_len;
    gpu_slots[slot].module_id    = next_module_id++;
    total_submitted++;

    log_drain_write(15, 15, "[gpu_scheduler] SUBMIT: slot=");
    char s[2] = {'0' + (char)slot, '\0'};
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = s; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " module_id="; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(15, 15, _cl_buf);
    }
    char m[2] = {'0' + (char)((gpu_slots[slot].module_id) % 10), '\0'};
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = m; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " ptx_len="; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(15, 15, _cl_buf);
    }
    /* Simple decimal print for ptx_len (up to 999999) */
    char nbuf[8];
    uint32_t n = ptx_len;
    int ni = 6;
    nbuf[7] = '\0';
    nbuf[6] = '\0';
    if (n == 0) { nbuf[ni--] = '0'; }
    while (n > 0 && ni >= 0) { nbuf[ni--] = '0' + (char)(n % 10); n /= 10; }
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = &nbuf[ni + 1]; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(15, 15, _cl_buf);
    }

    /*
     * On Sparky GB10 with nvrtc available, this would be:
     *   nvrtcProgram prog;
     *   nvrtcCreateProgram(&prog, ptx_src, "kernel.cu", 0, NULL, NULL);
     *   nvrtcCompileProgram(prog, 0, NULL);
     * For now: bookkeeping only (QEMU doesn't have a GPU).
     */

    rep_u32(rep, 0, GPU_OK);
    rep_u32(rep, 4, (uint32_t)slot);
    rep_u32(rep, 8, gpu_slots[slot].module_id);
    rep->length = 12;
        return SEL4_ERR_OK;
}

static uint32_t handle_complete(void) {
    IPC_STUB_LOCALS
    uint32_t slot_id = (uint32_t)msg_u32(req, 4);
    if (slot_id >= NUM_GPU_SLOTS) {
        rep_u32(rep, 0, GPU_ERR_BADSLOT);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
    if (!gpu_slots[slot_id].busy) {
        rep_u32(rep, 0, GPU_ERR_IDLE);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    log_drain_write(15, 15, "[gpu_scheduler] COMPLETE: slot=");
    char s[2] = {'0' + (char)slot_id, '\0'};
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = s; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(15, 15, _cl_buf);
    }

    gpu_slots[slot_id].busy      = false;
    gpu_slots[slot_id].module_id = 0;
    total_completed++;

    rep_u32(rep, 0, GPU_OK);
    rep->length = 4;
        return SEL4_ERR_OK;
}

static uint32_t handle_status(void) {
    IPC_STUB_LOCALS
    rep_u32(rep, 0, GPU_OK);
    rep_u32(rep, 4, busy_bitmask());
    rep_u32(rep, 8, total_submitted);
    rep_u32(rep, 12, total_completed);
    rep->length = 16;
        return SEL4_ERR_OK;
}

/* ── Microkit entrypoints ───────────────────────────────────────────── */

static void gpu_scheduler_pd_init(void) {
    log_drain_write(15, 15, "[gpu_scheduler] init — 4 GPU slots ready\n");
    for (int i = 0; i < NUM_GPU_SLOTS; i++) {
        gpu_slots[i].busy      = false;
        gpu_slots[i].module_id = 0;
    }
}

static uint32_t gpu_scheduler_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t op = (uint32_t)msg_u32(req, 0);

    switch (op) {
    case OP_GPU_SUBMIT:   return handle_submit();
    case OP_GPU_COMPLETE: return handle_complete();
    case OP_GPU_STATUS:   return handle_status();
    default:
        log_drain_write(15, 15, "[gpu_scheduler] unknown op\n");
        rep_u32(rep, 0, 0xFF);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
}

static void gpu_scheduler_pd_notified(uint32_t ch) {
    /* Notifications from vibe_engine on CUDA submit */
    if (ch == CH_VIBE) {
        uint32_t op = (uint32_t)msg_u32(req, 0);
        if (op == OP_GPU_SUBMIT) {
            handle_submit();
        }
    }
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void gpu_scheduler_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    gpu_scheduler_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, gpu_scheduler_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
