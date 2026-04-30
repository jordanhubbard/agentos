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
 *     Writes initial register state (PC, SP, startup args) into the TCB via
 *     seL4_TCB_WriteRegisters with resume=1 so the MCS thread is enqueued
 *     atomically after its scheduling context has been bound.
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
                               uint8_t    priority,
                               uint8_t    cnode_size_bits)
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
     * MCS seL4_TCB_Configure (7-arg form, gated by CONFIG_KERNEL_MCS):
     *   cspace_root      — extra cap[0]: PD's own CNode (CSpace root)
     *   cspace_root_data — MR[0]: guard+guardSize for the CNode cap.
     *                      MUST be non-zero: passing 0 means "has no effect"
     *                      in seL4 (the kernel treats it as "don't configure
     *                      the CSpace root"), leaving cspace_root as null.
     *                      Correct value: seL4_WordBits - cnode_size_bits,
     *                      which sets guardSize = (64 - size) bits covering
     *                      the upper address bits so cap addresses 0..2^size-1
     *                      map directly to their slot indices.
     *   vspace_root      — extra cap[1]: PD's VSpace
     *   vspace_root_data — MR[1]: 0 (no effect on ARM)
     *   buffer           — MR[2]: IPC buffer VA in PD's VSpace
     *   bufferFrame      — extra cap[2]: frame cap for IPC buffer page
     */
    seL4_Word cspace_root_data = (seL4_Word)(seL4_WordBits - (uint32_t)cnode_size_bits);
    err = seL4_TCB_Configure(tcb,
                              pd_cnode,          /* cspace_root */
                              cspace_root_data,  /* guard covers upper bits */
                              vspace_cap,        /* vspace_root */
                              0,                 /* vspace_root_data: no effect on ARM */
                              ipc_buf_va,        /* buffer VA in PD's VSpace */
                              ipc_buf_cap);      /* frame cap for IPC buffer */
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
                            seL4_Word arg0,
                            seL4_Word arg1)
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

    regs.AGENTOS_CTX_PC = entry; /* instruction pointer: ELF entry symbol */
#if defined(__x86_64__)
    /*
     * x86-64 C entry code expects the SysV ABI function-entry stack state:
     * %rsp is 8 mod 16, as if a caller had pushed a return address.  seL4
     * starts the thread directly at _start, so synthesize that alignment.
     */
    regs.AGENTOS_CTX_SP = sp - 8u;
#else
    regs.AGENTOS_CTX_SP = sp;    /* stack pointer: stack_top from loader  */
#endif
    regs.AGENTOS_CTX_ARG0 = arg0; /* first argument: my_ep CNode slot */
    regs.AGENTOS_CTX_ARG1 = arg1; /* second argument: ns_ep CNode slot */

    /*
     * Write the full register context to the TCB and atomically resume.
     *
     * resume = 1: transition the thread to ThreadState_Restart and enqueue it
     * in the scheduler's run queue immediately.  On seL4 MCS, WriteRegisters
     * with resume=0 followed by a separate seL4_TCB_Resume does NOT reliably
     * add freshly-typed Inactive threads to the run queue; resume=1 does so
     * atomically as part of the same kernel invocation, when the SC is already
     * bound (seL4_SchedContext_Bind has been called before this).
     *
     * arch_flags = 0: no arch-specific execution state flags.
     * count = full seL4_UserContext size in words.
     */
    return seL4_TCB_WriteRegisters(tcb_cap,
                                   1 /* resume: set Restart + SCHED_ENQUEUE */,
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
