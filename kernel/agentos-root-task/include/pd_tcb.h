/*
 * pd_tcb.h — TCB creation, scheduling, and thread lifecycle for protection domains
 *
 * The root task creates one seL4 TCB per protection domain during boot.  This
 * module encapsulates the three-step process:
 *
 *   1. pd_tcb_create  — allocate the TCB object and bind it to the PD's
 *                       CSpace, VSpace, and IPC buffer via seL4_TCB_Configure.
 *   2. pd_tcb_set_regs — write initial register state (PC, SP, startup args)
 *                        and atomically enqueue the thread via
 *                        seL4_TCB_WriteRegisters(resume=1).
 *   3. pd_tcb_start    — explicit resume for callers that still use the
 *                        older create/set_regs/start sequencing.
 *
 * Callers are responsible for allocating an IPC buffer frame and mapping it
 * into the PD's VSpace (see pd_vspace.h) before calling pd_tcb_create.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "sel4_boot.h"
#include "ut_alloc.h"
#include <stdint.h>

/* ── Result type ──────────────────────────────────────────────────────────── */

/*
 * pd_tcb_result_t — outcome of pd_tcb_create.
 *
 * Fields:
 *   tcb_cap   the capability to the newly-created TCB object;
 *             equals dest_slot on success, seL4_CapNull on failure
 *   error     seL4_NoError (0) on success, non-zero seL4_Error otherwise
 */
typedef struct {
    seL4_CPtr  tcb_cap;
    int        error;
} pd_tcb_result_t;

/* ── Functions ────────────────────────────────────────────────────────────── */

/*
 * pd_tcb_create — create and configure a TCB for a protection domain.
 *
 * Allocates a seL4_TCBObject via ut_alloc, then calls seL4_TCB_Configure to
 * bind the TCB to the PD's CNode, VSpace, and IPC buffer frame.  Finally,
 * sets the scheduling priority via seL4_TCB_SetPriority, using the root-task
 * TCB (seL4_CapInitThreadTCB) as the authority (MCP = 255).
 *
 * The thread is not started by create alone.  pd_tcb_set_regs writes the
 * initial context and enqueues the thread atomically.
 *
 * Parameters:
 *   dest_cnode    CNode in which to place the new TCB cap
 *                 (use seL4_CapInitThreadCNode for the root task's CSpace)
 *   dest_slot     slot index within dest_cnode for the TCB cap
 *   vspace_cap    the PD's VSpace capability (from pd_vspace_create)
 *   pd_cnode      the PD's own CNode cap (passed to seL4_TCB_Configure as
 *                 the thread's CSpace root)
 *   ipc_buf_cap   frame capability for the IPC buffer page
 *   ipc_buf_va    virtual address of the IPC buffer inside the PD's VSpace
 *   priority         scheduling priority to assign (0 = lowest, 255 = highest)
 *   cnode_size_bits  log2 of the PD's CNode slot count (e.g. 6 for 64 slots).
 *                    Used to set cspace_root_data = seL4_WordBits - cnode_size_bits,
 *                    which is the correct guard size for a flat single-level CSpace.
 *                    Passing 0 here would leave the guard unconfigured (seL4 treats
 *                    cspace_root_data=0 as "no effect"), breaking cap resolution.
 *
 * Returns:
 *   pd_tcb_result_t.tcb_cap  == dest_slot on success
 *   pd_tcb_result_t.error    == seL4_NoError (0) on success
 */
pd_tcb_result_t pd_tcb_create(seL4_CPtr  dest_cnode,
                               seL4_Word  dest_slot,
                               seL4_CPtr  vspace_cap,
                               seL4_CPtr  pd_cnode,
                               seL4_CPtr  ipc_buf_cap,
                               seL4_Word  ipc_buf_va,
                               uint8_t    priority,
                               uint8_t    cnode_size_bits);

/*
 * pd_tcb_set_regs — write initial register state for a PD thread.
 *
 * Zeroes the full seL4_UserContext, then sets:
 *   PC  = entry   (program counter / instruction pointer)
 *   SP  = sp      (initial stack pointer)
 *   ARG0 = arg0   (AArch64 x0 / RISC-V a0 / x86-64 rdi)
 *   ARG1 = arg1   (AArch64 x1 / RISC-V a1 / x86-64 rsi)
 *
 * resume=1 is passed to seL4_TCB_WriteRegisters so a freshly-typed MCS TCB is
 * enqueued atomically after its scheduling context has been bound.
 *
 * Parameters:
 *   tcb_cap    capability to the TCB to configure
 *   entry      virtual address of the PD's entry point (from pd_vspace_load_elf)
 *   sp         initial stack pointer (stack_top from pd_vspace_load_elf)
 *   arg0       value to place in the first argument register (my_ep CNode slot)
 *   arg1       value to place in the second argument register (ns_ep CNode slot)
 *
 * Returns seL4_NoError on success.
 */
seL4_Error pd_tcb_set_regs(seL4_CPtr tcb_cap,
                            seL4_Word entry,
                            seL4_Word sp,
                            seL4_Word arg0,
                            seL4_Word arg1);

/*
 * pd_tcb_start — make a configured PD thread runnable.
 *
 * Calls seL4_TCB_Resume on the given TCB capability.  The thread must have
 * been previously configured with pd_tcb_create and pd_tcb_set_regs.
 *
 * Returns seL4_NoError on success.
 */
seL4_Error pd_tcb_start(seL4_CPtr tcb_cap);

/*
 * pd_tcb_suspend — suspend a running PD thread.
 *
 * Calls seL4_TCB_Suspend on the given TCB capability.  The thread is removed
 * from the seL4 scheduler run-queue but retains all its capabilities and
 * register state.  It can be restarted by calling pd_tcb_start again.
 *
 * Returns seL4_NoError on success.
 */
seL4_Error pd_tcb_suspend(seL4_CPtr tcb_cap);
