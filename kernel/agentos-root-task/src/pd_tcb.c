/*
 * pd_tcb.c — TCB creation, scheduling parameter assignment, and PD thread start
 *
 * Implements the three-phase protection-domain thread lifecycle:
 *
 *   Phase 1 — pd_tcb_create:
 *     Allocates a seL4 TCB object via ut_alloc, binds it to the PD's CNode,
 *     VSpace, and IPC buffer via seL4_TCB_Configure, then sets the scheduling
 *     priority via seL4_TCB_SetPriority (authority = root-task TCB, MCP=255).
 *
 *   Phase 2 — pd_tcb_set_regs:
 *     Writes initial register state (PC, SP, first argument) into the TCB via
 *     seL4_TCB_WriteRegisters with resume=0 so the thread stays suspended.
 *
 *   Phase 3 — pd_tcb_start / pd_tcb_suspend:
 *     Transitions the thread between runnable and suspended states via
 *     seL4_TCB_Resume and seL4_TCB_Suspend respectively.
 *
 * No libc, no malloc, no global mutable state.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "pd_tcb.h"

/* ── Phase 1: allocation and configuration ─────────────────────────────────── */

pd_tcb_result_t pd_tcb_create(seL4_CPtr  dest_cnode,
                               seL4_Word  dest_slot,
                               seL4_CPtr  vspace_cap,
                               seL4_CPtr  pd_cnode,
                               seL4_CPtr  ipc_buf_cap,
                               seL4_Word  ipc_buf_va,
                               uint8_t    priority)
{
    seL4_Error err;

    /*
     * Allocate the TCB kernel object.
     *
     * seL4_TCBObject has a fixed size determined by the kernel; size_bits is
     * ignored for fixed-size objects, so we pass 0.  The resulting capability
     * lands at dest_cnode[dest_slot].
     */
    err = ut_alloc(seL4_TCBObject,
                   0 /* size_bits: ignored for TCBs */,
                   dest_cnode,
                   dest_slot,
                   64 /* dest_depth: 2^6-slot CNode */);
    if (err != seL4_NoError) {
        return (pd_tcb_result_t){ .tcb_cap = seL4_CapNull, .error = (int)err };
    }

    seL4_CPtr tcb = dest_slot;

    /*
     * Bind the TCB to the PD's address-space resources.
     *
     * seL4_TCB_Configure takes three extra capabilities via cap transfer:
     *   - cspace_root: pd_cnode  — the PD's own CNode (CSpace root)
     *   - vspace_root: vspace_cap — the PD's VSpace (page-table root)
     *   - bufferFrame: ipc_buf_cap — the IPC buffer page frame
     *
     * fault_ep = seL4_CapNull: no fault handler for now.  The monitor PD can
     * bind a fault endpoint later via a separate seL4_TCB_Configure call.
     *
     * cspace_root_data = 0: no CNode guard.
     * cspace_size_bits = 64: the PD CNode has 2^6 = 64 slots.
     * vspace_root_data = 0: no VSpace guard.
     */
    err = seL4_TCB_Configure(tcb,
                              pd_cnode,  /* cspace_root */
                              0          /* cspace_root_data */,
                              vspace_cap, /* vspace_root */
                              0          /* vspace_root_data */,
                              ipc_buf_va, /* buffer VA in PD's VSpace */
                              ipc_buf_cap /* frame cap for IPC buffer */);
    if (err != seL4_NoError) {
        return (pd_tcb_result_t){ .tcb_cap = seL4_CapNull, .error = (int)err };
    }

    /*
     * Set scheduling priority.
     *
     * The root task's TCB (seL4_CapInitThreadTCB) has MCP=255, so it can
     * grant any priority 0–255 to child threads.
     */
    err = seL4_TCB_SetPriority(tcb, seL4_CapInitThreadTCB, priority);
    if (err != seL4_NoError) {
        return (pd_tcb_result_t){ .tcb_cap = seL4_CapNull, .error = (int)err };
    }

    return (pd_tcb_result_t){ .tcb_cap = tcb, .error = (int)seL4_NoError };
}

/* ── Phase 2: initial register state ──────────────────────────────────────── */

seL4_Error pd_tcb_set_regs(seL4_CPtr tcb_cap,
                            seL4_Word entry,
                            seL4_Word sp,
                            seL4_Word arg0)
{
    /*
     * Zero the entire context first so that every unset register has a
     * deterministic value.  This avoids leaking root-task register state into
     * the new thread.
     */
    seL4_UserContext regs;
    for (uint32_t i = 0; i < sizeof(regs) / sizeof(seL4_Word); i++) {
        ((seL4_Word *)&regs)[i] = 0;
    }

    regs.pc = entry;  /* instruction pointer: ELF entry symbol          */
    regs.sp = sp;     /* stack pointer: stack_top from pd_vspace_load_elf */
    regs.AGENTOS_CTX_ARG0 = arg0; /* first argument (a0 on RISC-V, x0 on AArch64) */

    /*
     * Write the full register context to the TCB.
     *
     * resume = 0: keep the thread suspended; pd_tcb_start will resume it.
     * arch_flags = 0: no arch-specific execution state flags.
     * count = full seL4_UserContext size in words.
     */
    return seL4_TCB_WriteRegisters(tcb_cap,
                                   0 /* resume */,
                                   0 /* arch_flags */,
                                   (seL4_Word)(sizeof(seL4_UserContext) / sizeof(seL4_Word)),
                                   &regs);
}

/* ── Phase 3: thread lifecycle ─────────────────────────────────────────────── */

seL4_Error pd_tcb_start(seL4_CPtr tcb_cap)
{
    return seL4_TCB_Resume(tcb_cap);
}

seL4_Error pd_tcb_suspend(seL4_CPtr tcb_cap)
{
    return seL4_TCB_Suspend(tcb_cap);
}
