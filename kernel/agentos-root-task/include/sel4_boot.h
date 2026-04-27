/*
 * sel4_boot.h — seL4 bootstrap primitives used by the root task
 *
 * For production (bare-metal seL4) builds this header simply includes the
 * real seL4 SDK, which provides all types, cap-slot constants, object-type
 * constants, and inline invocations (seL4_TCB_Configure, seL4_TCB_SetPriority,
 * seL4_CNode_Mint, etc.) via LIBSEL4_INLINE_FUNC.
 *
 * For AGENTOS_TEST_HOST builds (no seL4 SDK available) this header provides
 * minimal stub types so host-side unit tests can compile without the SDK.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

#ifndef AGENTOS_TEST_HOST
/* ── Production build: pull in the real seL4 SDK ─────────────────────────────
 *
 * This gives us:
 *   seL4_Word / seL4_CPtr / seL4_Error (enum)
 *   seL4_CapNull, seL4_CapInitThreadTCB/CNode/VSpace, seL4_CapIRQControl, ...
 *   seL4_TCBObject, seL4_EndpointObject, ...
 *   inline seL4_TCB_Configure, seL4_TCB_SetPriority, seL4_TCB_WriteRegisters,
 *         seL4_TCB_Resume, seL4_TCB_Suspend, seL4_CNode_Mint,
 *         seL4_IRQControl_Get, seL4_IRQHandler_SetNotification, ...
 *   arch-specific seL4_UserContext with native field names (a0/x0/rdi etc.)
 */
#  include <sel4/sel4.h>

/*
 * Arch-specific alias for the first argument/return-value register in
 * seL4_UserContext.  Code that sets up a thread's initial argument uses
 * AGENTOS_CTX_ARG0 instead of a hardcoded field name.
 */
#  if defined(__riscv)
#    define AGENTOS_CTX_ARG0  a0   /* RISC-V: first argument register */
#  elif defined(__aarch64__)
#    define AGENTOS_CTX_ARG0  x0   /* AArch64: first argument register */
#  elif defined(__x86_64__)
#    define AGENTOS_CTX_ARG0  rdi  /* x86-64 SysV ABI: first integer argument */
#  else
#    define AGENTOS_CTX_ARG0  x0   /* fallback */
#  endif

#else /* AGENTOS_TEST_HOST ─────────────────────────────────────────────────── */

/* Minimal stubs — only what root-task and test code actually need. */

typedef uintptr_t  seL4_Word;
typedef seL4_Word  seL4_CPtr;
typedef uint32_t   seL4_Error;

/* Error codes */
#define seL4_NoError             ((seL4_Error)0u)
#define seL4_InvalidArgument     ((seL4_Error)1u)
#define seL4_InvalidCapability   ((seL4_Error)2u)
#define seL4_IllegalOperation    ((seL4_Error)3u)
#define seL4_RangeError          ((seL4_Error)4u)
#define seL4_AlignmentError      ((seL4_Error)5u)
#define seL4_FailedLookup        ((seL4_Error)6u)
#define seL4_TruncatedMessage    ((seL4_Error)7u)
#define seL4_DeleteFirst         ((seL4_Error)8u)
#define seL4_RevokeFirst         ((seL4_Error)9u)
#define seL4_NotEnoughMemory     ((seL4_Error)10u)

/* Object types */
#define seL4_TCBObject       1u
#define seL4_EndpointObject  4u

/* Cap slot constants (seL4 SDK enum seL4_RootCNodeCapSlots values) */
#define seL4_CapNull                ((seL4_CPtr)0u)
#define seL4_CapInitThreadTCB       ((seL4_CPtr)1u)
#define seL4_CapInitThreadCNode     ((seL4_CPtr)2u)
#define seL4_CapInitThreadVSpace    ((seL4_CPtr)3u)
#define seL4_CapIRQControl          ((seL4_CPtr)4u)
#define seL4_CapASIDControl         ((seL4_CPtr)5u)
#define seL4_CapInitThreadASIDPool  ((seL4_CPtr)6u)
#define seL4_CapIOPortControl       ((seL4_CPtr)7u)
#define seL4_CapBootInfoFrame       ((seL4_CPtr)9u)
#define seL4_CapInitThreadIPCBuffer ((seL4_CPtr)10u)
#define seL4_CapInitThreadSC        ((seL4_CPtr)14u) /* MCS: initial thread's scheduling context */

/*
 * seL4_UserContext — unified register layout for test-host builds.
 * Uses AArch64-style field names (x0, x1, ...).
 * Production builds use the arch-specific SDK struct.
 */
typedef struct {
    seL4_Word pc;
    seL4_Word sp;
    seL4_Word spsr;
    seL4_Word x0;
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
    seL4_Word x29;
    seL4_Word x30;
} seL4_UserContext;

#define AGENTOS_CTX_ARG0  x0
#define seL4_UserContext_n_regs  34u

/* Stub function declarations for test builds (no-op implementations in test CRT) */
seL4_Error seL4_TCB_Configure(seL4_CPtr  _service,
                               seL4_CPtr  cspace_root,
                               seL4_Word  cspace_root_data,
                               seL4_CPtr  vspace_root,
                               seL4_Word  vspace_root_data,
                               seL4_Word  buffer,
                               seL4_CPtr  bufferFrame);
seL4_Error seL4_TCB_SetPriority(seL4_CPtr _service,
                                 seL4_CPtr authority,
                                 seL4_Word priority);
seL4_Error seL4_TCB_WriteRegisters(seL4_CPtr           _service,
                                    uint8_t             resume,
                                    uint8_t             arch_flags,
                                    seL4_Word           count,
                                    seL4_UserContext   *regs);
seL4_Error seL4_TCB_Resume(seL4_CPtr _service);
seL4_Error seL4_TCB_Suspend(seL4_CPtr _service);
seL4_Error seL4_IRQControl_Get(seL4_CPtr  _service,
                                seL4_Word  irq,
                                seL4_CPtr  dest_root,
                                seL4_Word  dest_index,
                                seL4_Word  dest_depth);
seL4_Error seL4_IRQHandler_SetNotification(seL4_CPtr _service,
                                            seL4_CPtr notification);
seL4_Error seL4_IRQHandler_Ack(seL4_CPtr _service);
seL4_Error seL4_IRQHandler_Clear(seL4_CPtr _service);
seL4_Error seL4_CNode_Revoke(seL4_CPtr _service,
                              seL4_CPtr index,
                              uint32_t  depth);
seL4_Error seL4_CNode_Delete(seL4_CPtr _service,
                              seL4_CPtr index,
                              uint32_t  depth);
seL4_Error seL4_CNode_Mint(seL4_CPtr  service,
                            seL4_Word  dest_index,
                            uint8_t    dest_depth,
                            seL4_CPtr  src_root,
                            seL4_Word  src_index,
                            uint8_t    src_depth,
                            seL4_Word  rights,
                            seL4_Word  badge);

#endif /* AGENTOS_TEST_HOST */

/* ── VCPU register access (AArch64 virtualisation — arch-specific) ─────────── */

/*
 * seL4_VCPU_ReadRegs / seL4_VCPU_WriteRegs — AArch64 vCPU general-purpose
 * register access.  Used by VOS_SNAPSHOT and VOS_RESTORE.  Only available
 * on AArch64 hardware with virtualisation enabled; declared here for cross-arch
 * compilation; callers must guard usage with #ifdef __aarch64__.
 */
seL4_Error seL4_VCPU_ReadRegs(uint32_t  vcpu_cap,
                               uint32_t  regs[32],
                               uint64_t *pc);
seL4_Error seL4_VCPU_WriteRegs(uint32_t       vcpu_cap,
                                const uint32_t regs[32],
                                uint64_t       pc);
