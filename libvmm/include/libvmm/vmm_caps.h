/*
 * vmm_caps.h — seL4-native VMM capability table and VCPU operations
 *
 * Replaces the Microkit SDK's VCPU/notification API (microkit_vcpu_*,
 * microkit_notify, microkit_irq_ack) with raw seL4 invocations backed by
 * a capability table populated by the root task at PD bring-up.
 *
 * The Microkit SDK used fixed cap slot offsets (BASE_VCPU_CAP = 330,
 * BASE_VM_TCB_CAP = 266) assigned by the Microkit linker.  In raw seL4
 * we hold actual cap pointers handed down from the root task, stored in
 * g_vmm_vcpus[] and looked up by vcpu_id.
 *
 * Usage:
 *   Root task calls vmm_register_vcpu(id, vcpu_cap, tcb_cap) for each vCPU
 *   before the VMM PD's main() runs.  After that, all vmm_vcpu_* calls work.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <sel4/sel4.h>
#include <stddef.h>
#include <stdint.h>
/* assert.h is not available in all freestanding build environments; provide
 * a minimal fallback.  A real seL4 build will terminate on the seL4_Fail
 * syscall (debug builds) or silently continue (release builds). */
#ifndef assert
#define assert(x) ((void)(x))
#endif

/* SEL4_USER_CONTEXT_SIZE is defined in libvmm/util/util.h, but vmm_caps.h is
 * included before that in the libvmm.h include chain (virq.h → vmm_caps.h
 * comes before virtio.h → util.h).  Define it here to break the dependency. */
#ifndef SEL4_USER_CONTEXT_SIZE
#define SEL4_USER_CONTEXT_SIZE (sizeof(seL4_UserContext) / sizeof(seL4_Word))
#endif

/* ── Constants ───────────────────────────────────────────────────────────── */

#define VMM_MAX_VCPUS           8u
#define VMM_MAX_IRQ_HANDLERS    64u

/* ── VCPU capability table ───────────────────────────────────────────────── */

typedef struct {
    seL4_CPtr vcpu_cap;
    seL4_CPtr tcb_cap;
} vmm_vcpu_t;

extern vmm_vcpu_t g_vmm_vcpus[VMM_MAX_VCPUS];

static inline void vmm_register_vcpu(size_t id, seL4_CPtr vcpu_cap,
                                     seL4_CPtr tcb_cap)
{
    assert(id < VMM_MAX_VCPUS);
    g_vmm_vcpus[id].vcpu_cap = vcpu_cap;
    g_vmm_vcpus[id].tcb_cap  = tcb_cap;
}

static inline seL4_CPtr vmm_vcpu_cap(size_t vcpu_id)
{
    assert(vcpu_id < VMM_MAX_VCPUS);
    return g_vmm_vcpus[vcpu_id].vcpu_cap;
}

static inline seL4_CPtr vmm_tcb_cap(size_t vcpu_id)
{
    assert(vcpu_id < VMM_MAX_VCPUS);
    return g_vmm_vcpus[vcpu_id].tcb_cap;
}

/* ── VCPU register operations ────────────────────────────────────────────── */

static inline seL4_Word vmm_vcpu_arm_read_reg(size_t vcpu_id, seL4_Word reg)
{
    seL4_ARM_VCPU_ReadRegs_t ret = seL4_ARM_VCPU_ReadRegs(vmm_vcpu_cap(vcpu_id), reg);
    assert(ret.error == seL4_NoError);
    return ret.value;
}

static inline void vmm_vcpu_arm_write_reg(size_t vcpu_id, seL4_Word reg,
                                          seL4_Word value)
{
    seL4_Error err = seL4_ARM_VCPU_WriteRegs(vmm_vcpu_cap(vcpu_id), reg, value);
    assert(err == seL4_NoError);
    (void)err;
}

static inline void vmm_vcpu_arm_inject_irq(size_t vcpu_id, seL4_Uint16 irq,
                                           seL4_Uint8 priority,
                                           seL4_Uint8 group,
                                           seL4_Uint8 index)
{
    seL4_Error err = seL4_ARM_VCPU_InjectIRQ(vmm_vcpu_cap(vcpu_id),
                                              irq, priority, group, index);
    assert(err == seL4_NoError);
    (void)err;
}

static inline void vmm_vcpu_arm_ack_vppi(size_t vcpu_id, seL4_Word irq)
{
    seL4_Error err = seL4_ARM_VCPU_AckVPPI(vmm_vcpu_cap(vcpu_id), irq);
    assert(err == seL4_NoError);
    (void)err;
}

/* ── VCPU scheduling operations ──────────────────────────────────────────── */

static inline void vmm_vcpu_stop(size_t vcpu_id)
{
    seL4_Error err = seL4_TCB_Suspend(vmm_tcb_cap(vcpu_id));
    assert(err == seL4_NoError);
    (void)err;
}

/*
 * vmm_vcpu_restart — set the vCPU program counter to entry_point and resume.
 *
 * Reads the full register set, overwrites PC, then writes back with resume=1.
 * Used by PSCI CPU_ON to start secondary vCPUs.
 */
static inline void vmm_vcpu_restart(size_t vcpu_id, seL4_Word entry_point)
{
    seL4_UserContext regs;
    seL4_Error err = seL4_TCB_ReadRegisters(vmm_tcb_cap(vcpu_id), 0, 0,
                                             SEL4_USER_CONTEXT_SIZE, &regs);
    assert(err == seL4_NoError);
    regs.pc = entry_point;
    err = seL4_TCB_WriteRegisters(vmm_tcb_cap(vcpu_id), 1, 0,
                                   SEL4_USER_CONTEXT_SIZE, &regs);
    assert(err == seL4_NoError);
    (void)err;
}

/* ── IRQ and notification operations ─────────────────────────────────────── */

/* vmm_irq_ack — acknowledge an IRQ; irq_cap is the seL4 IRQ handler cap. */
static inline void vmm_irq_ack(seL4_CPtr irq_cap)
{
    seL4_IRQHandler_Ack(irq_cap);
}

/* vmm_notify — signal a notification cap (replaces microkit_notify). */
static inline void vmm_notify(seL4_CPtr ntfn_cap)
{
    seL4_Signal(ntfn_cap);
}

/* ── PD name ─────────────────────────────────────────────────────────────── */

/*
 * vmm_pd_name — null-terminated protection domain name.
 *
 * Each VMM PD must define this, e.g.:
 *   const char vmm_pd_name[] = "freebsd_vmm";
 */
extern const char vmm_pd_name[];

/* ── Debug output ─────────────────────────────────────────────────────────── */

static inline void vmm_dbg_puts(const char *s)
{
#ifdef CONFIG_PRINTING
    while (s && *s) seL4_DebugPutChar(*s++);
#else
    (void)s;
#endif
}
