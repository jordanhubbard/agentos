/*
 * sel4_boot.h — seL4 / Microkit bootstrap primitives used by the root task
 *
 * Wraps the seL4 TCB, CNode, and VSpace invocations needed during PD bring-up.
 * On a real seL4 build these types come from <sel4/sel4.h>; this header
 * provides the definitions when that header is not available (host-side tests,
 * stub builds).
 *
 * IMPORTANT: Do not call seL4 syscalls from the root task's protection-domain
 * event loop.  These invocations are legal only during boot initialisation,
 * before the Microkit runtime has started scheduling.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ── Primitive seL4 types ─────────────────────────────────────────────────── */

typedef uintptr_t  seL4_Word;
typedef seL4_Word  seL4_CPtr;

/* seL4 error codes — seL4_NoError == 0, all others are failures */
typedef uint32_t seL4_Error;

#define seL4_NoError    ((seL4_Error)0u)
#define seL4_InvalidArgument  ((seL4_Error)1u)
#define seL4_InvalidCapability ((seL4_Error)2u)
#define seL4_IllegalOperation  ((seL4_Error)3u)
#define seL4_RangeError        ((seL4_Error)4u)
#define seL4_AlignmentError    ((seL4_Error)5u)
#define seL4_FailedLookup      ((seL4_Error)6u)
#define seL4_TruncatedMessage  ((seL4_Error)7u)
#define seL4_DeleteFirst       ((seL4_Error)8u)
#define seL4_RevokeFirst       ((seL4_Error)9u)
#define seL4_NotEnoughMemory   ((seL4_Error)10u)

/* seL4 object types used at boot time */
#define seL4_TCBObject       1u
#define seL4_EndpointObject  4u
#define seL4_CapNull         ((seL4_CPtr)0u)

/* Well-known initial capability slots (Microkit-defined) */
#define seL4_CapInitThreadCNode  ((seL4_CPtr)1u)
#define seL4_CapInitThreadVSpace ((seL4_CPtr)2u)
#define seL4_CapInitThreadTCB    ((seL4_CPtr)3u)
#define seL4_CapIRQControl       ((seL4_CPtr)4u)
#define seL4_CapASIDControl      ((seL4_CPtr)5u)
#define seL4_CapInitThreadASIDPool ((seL4_CPtr)6u)
#define seL4_CapIOPortControl    ((seL4_CPtr)7u)
#define seL4_CapBootInfoFrame    ((seL4_CPtr)8u)

/* ── AArch64 / RISC-V / x86-64 user context ──────────────────────────────── */

/*
 * seL4_UserContext — register file written by seL4_TCB_WriteRegisters.
 *
 * Field layout follows the seL4 AArch64 ABI.  On RISC-V the names stay the
 * same (the assembler stubs map them appropriately).  Only pc, sp, and x0
 * (a0 on RISC-V) are architecturally significant for initial thread setup;
 * the remaining fields may be zero.
 *
 * Struct size is 34 * 8 = 272 bytes (34 general-purpose registers + PC + SP).
 */
typedef struct {
    seL4_Word pc;   /* program counter / instruction pointer         */
    seL4_Word sp;   /* stack pointer                                 */
    seL4_Word spsr; /* saved program status register (AArch64)       */
    seL4_Word x0;   /* first argument / return value register        */
    seL4_Word x1;
    seL4_Word x2;
    seL4_Word x3;
    seL4_Word x4;
    seL4_Word x5;
    seL4_Word x6;
    seL4_Word x7;
    seL4_Word x8;
    seL4_Word x9;
    seL4_Word x10;
    seL4_Word x11;
    seL4_Word x12;
    seL4_Word x13;
    seL4_Word x14;
    seL4_Word x15;
    seL4_Word x16;
    seL4_Word x17;
    seL4_Word x18;
    seL4_Word x19;
    seL4_Word x20;
    seL4_Word x21;
    seL4_Word x22;
    seL4_Word x23;
    seL4_Word x24;
    seL4_Word x25;
    seL4_Word x26;
    seL4_Word x27;
    seL4_Word x28;
    seL4_Word x29; /* frame pointer */
    seL4_Word x30; /* link register */
} seL4_UserContext;

#define seL4_UserContext_n_regs  34u  /* total registers in seL4_UserContext */

/* ── TCB invocations ──────────────────────────────────────────────────────── */

/*
 * seL4_TCB_Configure — bind a TCB to its CSpace, VSpace, and IPC buffer.
 *
 * Parameters:
 *   _service      TCB capability to configure
 *   fault_ep      fault endpoint (seL4_CapNull = no fault handler)
 *   cspace_root   the PD's CNode capability
 *   cspace_root_data  guard value for the CNode (normally 0)
 *   cspace_size_bits  radix of the CNode (e.g. 64 for a 2^6-slot CNode)
 *   vspace_root   the PD's VSpace (PageDirectory / PageTable root)
 *   vspace_root_data  guard for the VSpace (normally 0)
 *   buffer        virtual address of the IPC buffer inside the PD's VSpace
 *   bufferFrame   frame capability mapped at that virtual address
 *
 * Returns seL4_NoError on success.
 */
seL4_Error seL4_TCB_Configure(seL4_CPtr  _service,
                               seL4_CPtr  fault_ep,
                               seL4_CPtr  cspace_root,
                               seL4_Word  cspace_root_data,
                               seL4_Word  cspace_size_bits,
                               seL4_CPtr  vspace_root,
                               seL4_Word  vspace_root_data,
                               seL4_Word  buffer,
                               seL4_CPtr  bufferFrame);

/*
 * seL4_TCB_SetPriority — set the scheduling priority of a TCB.
 *
 * Parameters:
 *   _service    TCB capability to update
 *   authority   TCB cap whose MCP (maximum controlled priority) authorises
 *               the assignment (root task uses seL4_CapInitThreadTCB)
 *   priority    new scheduling priority (0 = lowest, 255 = highest)
 *
 * Returns seL4_NoError on success.
 */
seL4_Error seL4_TCB_SetPriority(seL4_CPtr _service,
                                 seL4_CPtr authority,
                                 uint8_t   priority);

/*
 * seL4_TCB_WriteRegisters — write general-purpose registers to a TCB.
 *
 * Parameters:
 *   _service    TCB capability to update
 *   resume      if non-zero, resume the thread after writing
 *   arch_flags  architecture-specific flags (normally 0)
 *   count       number of registers to write from regs
 *   regs        pointer to the register context (seL4_UserContext)
 *
 * Returns seL4_NoError on success.
 */
seL4_Error seL4_TCB_WriteRegisters(seL4_CPtr           _service,
                                    uint8_t             resume,
                                    uint8_t             arch_flags,
                                    seL4_Word           count,
                                    seL4_UserContext   *regs);

/*
 * seL4_TCB_Resume — make a suspended thread runnable.
 *
 * Returns seL4_NoError on success.
 */
seL4_Error seL4_TCB_Resume(seL4_CPtr _service);

/*
 * seL4_TCB_Suspend — suspend a running thread.
 *
 * The thread is removed from the scheduler run-queue.  It can be restarted
 * later with seL4_TCB_Resume or seL4_TCB_WriteRegisters(resume=1).
 *
 * Returns seL4_NoError on success.
 */
seL4_Error seL4_TCB_Suspend(seL4_CPtr _service);

/* ── IRQ capability invocations ───────────────────────────────────────────── */

/*
 * seL4_IRQControl_Get — obtain an IRQ handler capability for one hardware IRQ.
 *
 * The kernel places a new IRQ handler capability into dest_cnode[dest_index]
 * interpreted at dest_depth bits of address.
 *
 * Parameters:
 *   _service      IRQ control capability (seL4_CapIRQControl)
 *   irq           hardware IRQ number (GIC SPI number on AArch64)
 *   dest_root     CNode in which to place the new capability
 *   dest_index    slot within dest_root
 *   dest_depth    number of bits of address resolved in dest_root (e.g. 8 for
 *                 a 256-slot CNode, 6 for a 64-slot CNode)
 *
 * Returns seL4_NoError on success.
 */
seL4_Error seL4_IRQControl_Get(seL4_CPtr  _service,
                                seL4_Word  irq,
                                seL4_CPtr  dest_root,
                                seL4_Word  dest_index,
                                seL4_Word  dest_depth);

/*
 * seL4_IRQHandler_SetNotification — bind a notification object to an IRQ handler.
 *
 * When the hardware IRQ fires, seL4 signals the notification with the given
 * badge.  The PD waiting on the notification receives a badged signal.
 *
 * Parameters:
 *   _service      IRQ handler capability (obtained via seL4_IRQControl_Get)
 *   notification  notification capability to signal on IRQ
 *
 * Returns seL4_NoError on success.
 */
seL4_Error seL4_IRQHandler_SetNotification(seL4_CPtr _service,
                                            seL4_CPtr notification);

/*
 * seL4_IRQHandler_Ack — acknowledge a handled IRQ, re-enabling it in the GIC.
 *
 * Must be called after each IRQ is handled.  Until Ack is called, the GIC will
 * not deliver further instances of this IRQ to the CPU.
 *
 * Parameters:
 *   _service      IRQ handler capability
 *
 * Returns seL4_NoError on success.
 */
seL4_Error seL4_IRQHandler_Ack(seL4_CPtr _service);

/*
 * seL4_IRQHandler_Clear — detach the notification from an IRQ handler and
 * mask the IRQ in the GIC.  Call before deleting an IRQ handler capability.
 *
 * Parameters:
 *   _service      IRQ handler capability
 *
 * Returns seL4_NoError on success.
 */
seL4_Error seL4_IRQHandler_Clear(seL4_CPtr _service);

/*
 * seL4_CNode_Revoke — revoke all derived capabilities of the cap at
 * _service[index].  Must be called before seL4_CNode_Delete when tearing
 * down a guest capability tree to avoid dangling child caps.
 *
 * Parameters:
 *   _service   CNode capability (root CNode of the slot to revoke from)
 *   index      slot index within _service
 *   depth      radix of _service in bits (typically 64 for the root CNode)
 *
 * Returns seL4_NoError on success.
 */
seL4_Error seL4_CNode_Revoke(seL4_CPtr _service,
                              seL4_CPtr index,
                              uint32_t  depth);

/*
 * seL4_CNode_Delete — delete the capability stored at _service[index],
 * freeing the slot.  Call seL4_CNode_Revoke first if the cap has children.
 *
 * Parameters:
 *   _service   CNode capability
 *   index      slot index within _service
 *   depth      radix of _service in bits (typically 64 for the root CNode)
 *
 * Returns seL4_NoError on success.
 */
seL4_Error seL4_CNode_Delete(seL4_CPtr _service,
                              seL4_CPtr index,
                              uint32_t  depth);

/*
 * seL4_VCPU_ReadRegs / seL4_VCPU_WriteRegs — AArch64 vCPU general-purpose
 * register access.  Used by VOS_SNAPSHOT and VOS_RESTORE to capture and
 * replay guest CPU state without going through a full context switch.
 *
 * Parameters:
 *   vcpu_cap   VCPU capability index
 *   regs       array of 32 × 32-bit general-purpose register values (x0-x30 + SP)
 *   pc         program counter (64-bit)
 *
 * seL4_VCPU_ReadRegs returns seL4_NoError on success; writes regs[] and *pc.
 * seL4_VCPU_WriteRegs returns seL4_NoError on success.
 */
seL4_Error seL4_VCPU_ReadRegs(uint32_t  vcpu_cap,
                               uint32_t  regs[32],
                               uint64_t *pc);
seL4_Error seL4_VCPU_WriteRegs(uint32_t       vcpu_cap,
                                const uint32_t regs[32],
                                uint64_t       pc);
