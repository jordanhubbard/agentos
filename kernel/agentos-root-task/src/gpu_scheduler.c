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

#include <microkit.h>
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

static microkit_msginfo handle_submit(void) {
    uint32_t prop_slot  = (uint32_t)microkit_mr_get(1);
    uint32_t ptx_offset = (uint32_t)microkit_mr_get(2);
    uint32_t ptx_len    = (uint32_t)microkit_mr_get(3);

    int slot = find_free_slot();
    if (slot < 0) {
        console_log(15, 15, "[gpu_scheduler] REJECT: all 4 GPU slots busy\n");
        microkit_mr_set(0, GPU_ERR_FULL);
        return microkit_msginfo_new(0, 1);
    }

    gpu_slots[slot].busy         = true;
    gpu_slots[slot].proposal_slot = prop_slot;
    gpu_slots[slot].ptx_offset   = ptx_offset;
    gpu_slots[slot].ptx_len      = ptx_len;
    gpu_slots[slot].module_id    = next_module_id++;
    total_submitted++;

    console_log(15, 15, "[gpu_scheduler] SUBMIT: slot=");
    char s[2] = {'0' + (char)slot, '\0'};
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = s; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " module_id="; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(15, 15, _cl_buf);
    }
    char m[2] = {'0' + (char)((gpu_slots[slot].module_id) % 10), '\0'};
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = m; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " ptx_len="; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(15, 15, _cl_buf);
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
        console_log(15, 15, _cl_buf);
    }

    /*
     * On Sparky GB10 with nvrtc available, this would be:
     *   nvrtcProgram prog;
     *   nvrtcCreateProgram(&prog, ptx_src, "kernel.cu", 0, NULL, NULL);
     *   nvrtcCompileProgram(prog, 0, NULL);
     * For now: bookkeeping only (QEMU doesn't have a GPU).
     */

    microkit_mr_set(0, GPU_OK);
    microkit_mr_set(1, (uint32_t)slot);
    microkit_mr_set(2, gpu_slots[slot].module_id);
    return microkit_msginfo_new(0, 3);
}

static microkit_msginfo handle_complete(void) {
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);
    if (slot_id >= NUM_GPU_SLOTS) {
        microkit_mr_set(0, GPU_ERR_BADSLOT);
        return microkit_msginfo_new(0, 1);
    }
    if (!gpu_slots[slot_id].busy) {
        microkit_mr_set(0, GPU_ERR_IDLE);
        return microkit_msginfo_new(0, 1);
    }

    console_log(15, 15, "[gpu_scheduler] COMPLETE: slot=");
    char s[2] = {'0' + (char)slot_id, '\0'};
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = s; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(15, 15, _cl_buf);
    }

    gpu_slots[slot_id].busy      = false;
    gpu_slots[slot_id].module_id = 0;
    total_completed++;

    microkit_mr_set(0, GPU_OK);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_status(void) {
    microkit_mr_set(0, GPU_OK);
    microkit_mr_set(1, busy_bitmask());
    microkit_mr_set(2, total_submitted);
    microkit_mr_set(3, total_completed);
    return microkit_msginfo_new(0, 4);
}

/* ── Microkit entrypoints ───────────────────────────────────────────── */

void init(void) {
    console_log(15, 15, "[gpu_scheduler] init — 4 GPU slots ready\n");
    for (int i = 0; i < NUM_GPU_SLOTS; i++) {
        gpu_slots[i].busy      = false;
        gpu_slots[i].module_id = 0;
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)msginfo;
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
    case OP_GPU_SUBMIT:   return handle_submit();
    case OP_GPU_COMPLETE: return handle_complete();
    case OP_GPU_STATUS:   return handle_status();
    default:
        console_log(15, 15, "[gpu_scheduler] unknown op\n");
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }
}

void notified(microkit_channel ch) {
    /* Notifications from vibe_engine on CUDA submit */
    if (ch == CH_VIBE) {
        uint32_t op = (uint32_t)microkit_mr_get(0);
        if (op == OP_GPU_SUBMIT) {
            handle_submit();
        }
    }
}
