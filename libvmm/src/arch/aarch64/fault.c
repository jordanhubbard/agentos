/*
 * Copyright 2017, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/vmm_caps.h>
#include <libvmm/util/util.h>
#include <libvmm/tcb.h>
#include <libvmm/vcpu.h>
#include <libvmm/arch/aarch64/cpuif.h>
#include <libvmm/arch/aarch64/hsr.h>
#include <libvmm/arch/aarch64/smc.h>
#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>

// #define CPSR_THUMB                 (1 << 5)
// #define CPSR_IS_THUMB(x)           ((x) & CPSR_THUMB)

// int fault_is_32bit_instruction(seL4_UserContext *regs)
// {
//     // @ivanv: assuming VCPU fault
//     return !CPSR_IS_THUMB(regs->spsr);
// }

bool fault_advance_vcpu(size_t vcpu_id, seL4_UserContext *regs)
{
    // For now we just ignore it and continue
    // Assume 32-bit instruction
    regs->pc += 4;
    /*
     * Do not explicitly resume the TCB because we will eventually reply to the
     * fault which will result in the TCB being restarted.
     */
    int err = seL4_TCB_WriteRegisters(vmm_tcb_cap(vcpu_id), false, 0, SEL4_USER_CONTEXT_SIZE, regs);
    assert(err == seL4_NoError);

    return (err == seL4_NoError);
}

char *fault_to_string(seL4_Word fault_label)
{
    switch (fault_label) {
    case seL4_Fault_VMFault:
        return "virtual memory";
    case seL4_Fault_UnknownSyscall:
        return "unknown syscall";
    case seL4_Fault_UserException:
        return "user exception";
    case seL4_Fault_VGICMaintenance:
        return "VGIC maintenance";
    case seL4_Fault_VCPUFault:
        return "VCPU";
    case seL4_Fault_VPPIEvent:
        return "VPPI event";
    default:
        return "unknown fault";
    }
}

/*
 * Possible 'Syndrome Access Size' defined in ARMv8-A specification. The values
 * of the enums line up with the SAS encoding of the ISS encoding for an
 * exception from a Data Abort.
 */
enum fault_width {
    WIDTH_BYTE = 0b00,
    WIDTH_HALFWORD = 0b01,
    WIDTH_WORD = 0b10,
    WIDTH_DOUBLEWORD = 0b11,
};

static enum fault_width fault_get_width(uint64_t fsr)
{
    if (HSR_IS_SYNDROME_VALID(fsr) && HSR_SYNDROME_WIDTH(fsr) <= WIDTH_DOUBLEWORD) {
        return (enum fault_width)(HSR_SYNDROME_WIDTH(fsr));
    } else {
        LOG_VMM_ERR("Received invalid FSR: 0x%lx\n", fsr);
        // @ivanv: reviist
        // int rt;
        // rt = decode_instruction(f);
        // assert(rt >= 0);
        return -1;
    }
}

uint64_t fault_get_data_mask(uint64_t addr, uint64_t fsr)
{
    uint64_t mask = 0;
    switch (fault_get_width(fsr)) {
    case WIDTH_BYTE:
        mask = 0x000000ff;
        assert(!(addr & 0x0));
        break;
    case WIDTH_HALFWORD:
        mask = 0x0000ffff;
        assert(!(addr & 0x1));
        break;
    case WIDTH_WORD:
        mask = 0xffffffff;
        assert(!(addr & 0x3));
        break;
    case WIDTH_DOUBLEWORD:
        mask = ~mask;
        break;
    default:
        LOG_VMM_ERR("unknown width: 0x%lx, from FSR: 0x%lx, addr: 0x%lx\n",
                    fault_get_width(fsr), fsr, addr);
        assert(0);
        return 0;
    }
    mask <<= (addr & 0x3) * 8;
    return mask;
}

static seL4_Word wzr = 0;
seL4_Word *decode_rt(size_t reg_idx, seL4_UserContext *regs)
{
    /*
     * This switch statement is necessary due to a mismatch between how
     * seL4 orders the TCB registers compared to what the architecture
     * encodes the Syndrome Register transfer.
     */
    switch (reg_idx) {
    case 0:
        return &regs->x0;
    case 1:
        return &regs->x1;
    case 2:
        return &regs->x2;
    case 3:
        return &regs->x3;
    case 4:
        return &regs->x4;
    case 5:
        return &regs->x5;
    case 6:
        return &regs->x6;
    case 7:
        return &regs->x7;
    case 8:
        return &regs->x8;
    case 9:
        return &regs->x9;
    case 10:
        return &regs->x10;
    case 11:
        return &regs->x11;
    case 12:
        return &regs->x12;
    case 13:
        return &regs->x13;
    case 14:
        return &regs->x14;
    case 15:
        return &regs->x15;
    case 16:
        return &regs->x16;
    case 17:
        return &regs->x17;
    case 18:
        return &regs->x18;
    case 19:
        return &regs->x19;
    case 20:
        return &regs->x20;
    case 21:
        return &regs->x21;
    case 22:
        return &regs->x22;
    case 23:
        return &regs->x23;
    case 24:
        return &regs->x24;
    case 25:
        return &regs->x25;
    case 26:
        return &regs->x26;
    case 27:
        return &regs->x27;
    case 28:
        return &regs->x28;
    case 29:
        return &regs->x29;
    case 30:
        return &regs->x30;
    case 31:
        return &wzr;
    default:
        LOG_VMM_ERR("failed to decode Rt, attempted to access invalid register index 0x%lx\n", reg_idx);
        return NULL;
    }
}

bool fault_is_write(uint64_t fsr)
{
    return (fsr & (1U << 6)) != 0;
}

bool fault_is_read(uint64_t fsr)
{
    return !fault_is_write(fsr);
}

static int get_rt(uint64_t fsr)
{
    int rt = -1;
    if (HSR_IS_SYNDROME_VALID(fsr)) {
        rt = HSR_SYNDROME_RT(fsr);
    } else {
        printf("decode_insturction for AArch64 not implemented\n");
        assert(0);
        // @ivanv: implement decode instruction for aarch64
        // rt = decode_instruction(f);
    }
    assert(rt >= 0);
    return rt;
}

uint64_t fault_get_data(seL4_UserContext *regs, uint64_t fsr)
{
    /* Get register opearand */
    int rt = get_rt(fsr);

    uint64_t data = *decode_rt(rt, regs);

    return data;
}

uint64_t fault_emulate(seL4_UserContext *regs, uint64_t reg, uint64_t addr, uint64_t fsr, uint64_t reg_val)
{
    uint64_t m, s;
    s = (addr & 0x3) * 8;
    m = fault_get_data_mask(addr, fsr);
    if (fault_is_read(fsr)) {
        /* Read data must be shifted to lsb */
        return (reg & ~(m >> s)) | ((reg_val & m) >> s);
    } else {
        /* Data to write must be shifted left to compensate for alignment */
        return (reg & ~m) | ((reg_val << s) & m);
    }
}

void fault_emulate_write(seL4_UserContext *regs, size_t addr, size_t fsr, size_t reg_val)
{
    // @ivanv: audit
    /* Get register opearand */
    int rt = get_rt(fsr);
    seL4_Word *reg_ctx = decode_rt(rt, regs);
    *reg_ctx = fault_emulate(regs, *reg_ctx, addr, fsr, reg_val);
}

bool fault_advance(size_t vcpu_id, seL4_UserContext *regs, uint64_t addr, uint64_t fsr, uint64_t reg_val)
{
    /* Get register opearand */
    int rt = get_rt(fsr);

    seL4_Word *reg_ctx = decode_rt(rt, regs);
    *reg_ctx = fault_emulate(regs, *reg_ctx, addr, fsr, reg_val);

    return fault_advance_vcpu(vcpu_id, regs);
}

bool fault_handle_vcpu_exception(size_t vcpu_id)
{
    uint64_t hsr = seL4_GetMR(seL4_VCPUFault_HSR);
    uint64_t hsr_ec_class = HSR_EXCEPTION_CLASS(hsr);

    seL4_UserContext regs;
    int err = seL4_TCB_ReadRegisters(vmm_tcb_cap(vcpu_id), false, 0, SEL4_USER_CONTEXT_SIZE, &regs);
    assert(err == seL4_NoError);

    switch (hsr_ec_class) {
    case HSR_SMC_64_EXCEPTION:
    case HSR_HVC_64_EXCEPTION:
        return smc_handle(vcpu_id, hsr);
    case HSR_WFx_EXCEPTION:
        // If we get a WFI exception, we just do nothing in the VMM.
        return true;
    case HSR_SYSREG_64_EXCEPTION:
        return handle_sysreg_64_fault(vcpu_id, hsr, &regs);
    default:
        LOG_VMM_ERR("unknown vCPU exception, vCPU ID %lu, EC class: 0x%lx, HSR: 0x%lx\n", vcpu_id, hsr_ec_class, hsr);
        return false;
    }
}

bool fault_handle_vppi_event(size_t vcpu_id)
{
    static uint64_t vppi_count = 0;
    vppi_count++;
    uint64_t ppi_irq = seL4_GetMR(seL4_VPPIEvent_IRQ);

    /* Dump VCPU state to diagnose deadlocks */
    seL4_UserContext regs = {0};
    seL4_TCB_ReadRegisters(vmm_tcb_cap(vcpu_id), 0, 0, SEL4_USER_CONTEXT_SIZE, &regs);
    seL4_Word spsr_el1   = vmm_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_SPSR_EL1);
    seL4_Word elr_el1    = vmm_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_ELR_EL1);
    seL4_Word cntv_ctl   = vmm_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CNTV_CTL);
    seL4_Word cntv_cval  = vmm_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CNTV_CVAL);
    LOG_VMM("VPPIEvent #%llu: IRQ %llu on vCPU %zu\n",
            (unsigned long long)vppi_count,
            (unsigned long long)ppi_irq,
            vcpu_id);
    LOG_VMM("VPPIEvent #%llu: PC=0x%lx pstate=0x%lx (I=%s)\n",
            (unsigned long long)vppi_count,
            (unsigned long)regs.pc, (unsigned long)regs.spsr,
            (regs.spsr >> 7) & 1 ? "masked" : "enabled");
    LOG_VMM("VPPIEvent #%llu: SPSR_EL1=0x%lx ELR_EL1=0x%lx\n",
            (unsigned long long)vppi_count,
            (unsigned long)spsr_el1, (unsigned long)elr_el1);
    LOG_VMM("VPPIEvent #%llu: CNTV_CTL=0x%lx CNTV_CVAL=0x%lx\n",
            (unsigned long long)vppi_count,
            (unsigned long)cntv_ctl, (unsigned long)cntv_cval);
    /*
     * I=masked + ISTATUS=1: VCPU is inside an ISR (I=masked) with the
     * virtual timer expired.
     *
     * Root cause of the VPPIEvent flood: after VGICMaintenance clears the
     * LR and calls AckVPPI (unmasking the physical GIC), CNTV_CVAL is still
     * expired, so the hardware re-fires IRQ 27 before the guest can execute
     * even one instruction.  The ISR fires at PC=0x57dc0b14, which is the
     * `ret` at the end of MmioWrite32() called from EndOfInterrupt().  After
     * that `ret` returns, the ISR checks `if (CNTV_CTL & ISTATUS)` and, if
     * ISTATUS=1, calls the notify callbacks then advances CNTV_CVAL past
     * CNTVCT with `do { cval += mTimerTicks; } while (cval < cntvct);`.
     *
     * Fix (v25): inject IRQ 27 into a fresh LR (pends silently while
     * I=masked) and skip AckVPPI.  The GIC stays masked, so no new
     * VPPIEvent can fire.  After `ret`, the ISR sees ISTATUS=1, runs its
     * update body, and writes the correct near-future CNTV_CVAL.  On ERET
     * with I=enabled the LR fires a second ISR invocation; that invocation's
     * EndOfInterrupt → Maintenance → virq_ack → AckVPPI unmasks the GIC,
     * restoring normal timer delivery with CVAL already set to the next tick.
     *
     * Prior failures:
     *  v21: ENABLE=0 + AckVPPI → ISR sees ISTATUS=0 (ENABLE=0 clears it),
     *       CVAL never updated, callbacks spin `while (!ISTATUS)` forever.
     *  v22: AckVPPI only, CVAL unchanged → 134K VPPIEvents/s flood.
     *  v23: MRS cntvct_el0 from EL0 → traps (seL4 does not set CNTHCTL_EL2
     *       to allow EL0 virtual-counter access) → VMM crash.
     *  v24: advance CVAL to far-future → ISR sees ISTATUS=0, skips its
     *       update body, CVAL stays huge, timer dead for ~544 real seconds.
     */
    if (((regs.spsr >> 7) & 1) && (cntv_ctl & 4)) {
        LOG_VMM("VPPIEvent #%llu: I=masked+ISTATUS=1 at PC=0x%lx -- inject+defer AckVPPI (CVAL unchanged)\n",
                (unsigned long long)vppi_count, (unsigned long)regs.pc);
        bool ok = vgic_inject_irq(vcpu_id, ppi_irq);
        if (!ok) {
            LOG_VMM_ERR("VPPIEvent #%llu: no free LR, falling back to AckVPPI\n",
                        (unsigned long long)vppi_count);
            vmm_vcpu_arm_ack_vppi(vcpu_id, ppi_irq);
        }
        /* Skip AckVPPI: GIC stays masked until Maintenance → virq_ack. */
        return true;
    }
    bool success = vgic_inject_irq(vcpu_id, ppi_irq);
    LOG_VMM("VPPIEvent #%llu: inject_irq(%llu) returned %s\n",
            (unsigned long long)vppi_count,
            (unsigned long long)ppi_irq,
            success ? "OK" : "DROPPED");
    if (!success) {
        LOG_VMM_ERR("VPPI IRQ %lu dropped on vCPU %d -- acking VPPI to unmask\n", ppi_irq, vcpu_id);
        vmm_vcpu_arm_ack_vppi(vcpu_id, ppi_irq);
    }

    return true;
}

bool fault_handle_user_exception(size_t vcpu_id)
{
    // @ivanv: print out VM name/vCPU id when we have multiple VMs
    size_t fault_ip = seL4_GetMR(seL4_UserException_FaultIP);
    size_t number = seL4_GetMR(seL4_UserException_Number);
    LOG_VMM_ERR("Invalid instruction fault at IP: 0x%lx, number: 0x%lx from vCPU %d\n", fault_ip, number, vcpu_id);
    /* All we do is dump the TCB registers. */
    tcb_print_regs(vcpu_id);

    return false;
}

// @ivanv: document where these come from
#define SYSCALL_PA_TO_IPA 65
#define SYSCALL_NOP 67

bool fault_handle_unknown_syscall(size_t vcpu_id)
{
    // @ivanv: should print out the name of the VM the fault came from.
    size_t syscall = seL4_GetMR(seL4_UnknownSyscall_Syscall);
    size_t fault_ip = seL4_GetMR(seL4_UnknownSyscall_FaultIP);

    LOG_VMM("Received syscall 0x%lx\n", syscall);
    switch (syscall) {
    case SYSCALL_PA_TO_IPA:
        // @ivanv: why do we not do anything here?
        // @ivanv, how to get the physical address to translate?
        LOG_VMM("Received PA translation syscall\n");
        break;
    case SYSCALL_NOP:
        LOG_VMM("Received NOP syscall\n");
        break;
    default:
        LOG_VMM_ERR("Unknown syscall: syscall number: 0x%lx, PC: 0x%lx\n", syscall, fault_ip);
        return false;
    }

    seL4_UserContext regs;
    seL4_Error err = seL4_TCB_ReadRegisters(vmm_tcb_cap(vcpu_id), false, 0, SEL4_USER_CONTEXT_SIZE, &regs);
    assert(err == seL4_NoError);
    if (err != seL4_NoError) {
        LOG_VMM_ERR("Failure reading TCB registers when handling unknown syscall, error %d", err);
        return false;
    }

    return fault_advance_vcpu(vcpu_id, &regs);
}

#define MAX_VM_EXCEPTION_HANDLERS 16

struct vm_exception_handler {
    uintptr_t base;
    uintptr_t end;
    vm_exception_handler_t callback;
    void *data;
};

static struct vm_exception_handler registered_vm_exception_handlers[MAX_VM_EXCEPTION_HANDLERS];
static size_t vm_exception_handler_index = 0;

bool fault_register_vm_exception_handler(uintptr_t base, size_t size, vm_exception_handler_t callback, void *data)
{

    if (vm_exception_handler_index == MAX_VM_EXCEPTION_HANDLERS - 1) {
        LOG_VMM_ERR("maximum number of VM exception handlers registered");
        return false;
    }

    if (size == 0) {
        LOG_VMM_ERR("registered VM exception handler with size 0\n");
        return false;
    }

    for (int i = 0; i < vm_exception_handler_index; i++) {
        struct vm_exception_handler *curr = &registered_vm_exception_handlers[i];
        if (!(base >= curr->end || base + size <= curr->base)) {
            LOG_VMM_ERR("VM exception handler [0x%lx..0x%lx), overlaps with another handler [0x%lx..0x%lx)\n",
                        base, base + size, curr->base, curr->end);
            return false;
        }
    }

    registered_vm_exception_handlers[vm_exception_handler_index] = (struct vm_exception_handler) {
        .base = base,
        .end = base + size,
        .callback = callback,
        .data = data,
    };
    vm_exception_handler_index += 1;

    return true;
}

static bool fault_handle_registered_vm_exceptions(size_t vcpu_id, uintptr_t addr, size_t fsr, seL4_UserContext *regs)
{
    for (int i = 0; i < MAX_VM_EXCEPTION_HANDLERS; i++) {
        uintptr_t base = registered_vm_exception_handlers[i].base;
        uintptr_t end = registered_vm_exception_handlers[i].end;
        vm_exception_handler_t callback = registered_vm_exception_handlers[i].callback;
        void *data = registered_vm_exception_handlers[i].data;
        if (addr >= base && addr < end) {
            bool success = callback(vcpu_id, addr - base, fsr, regs, data);
            if (!success) {
                LOG_VMM_ERR("registered virtual memory exception handler for region [0x%lx..0x%lx) at address 0x%lx failed\n", base,
                            end, addr);
            }

            return success;
        }
    }

    /* We could not find a handler for the faulting address. */
    return false;
}

bool fault_handle_vm_exception(size_t vcpu_id)
{
    uintptr_t addr = seL4_GetMR(seL4_VMFault_Addr);
    size_t fsr = seL4_GetMR(seL4_VMFault_FSR);

    seL4_UserContext regs;
    int err = seL4_TCB_ReadRegisters(vmm_tcb_cap(vcpu_id), false, 0, SEL4_USER_CONTEXT_SIZE, &regs);
    assert(err == seL4_NoError);

    bool success = fault_handle_registered_vm_exceptions(vcpu_id, addr, fsr, &regs);
    if (!success) {
        /*
         * We could not find a registered handler for the address, meaning that the fault
         * is genuinely unexpected. Surprise!
         * Now we print out as much information relating to the fault as we can, hopefully
         * the programmer can figure out what went wrong.
         */
        size_t ip = seL4_GetMR(seL4_VMFault_IP);
        size_t is_prefetch = seL4_GetMR(seL4_VMFault_PrefetchFault);
        bool is_write = fault_is_write(fsr);
        LOG_VMM_ERR("unexpected memory fault on address: 0x%lx, FSR: 0x%lx, IP: 0x%lx, is_prefetch: %s, is_write: %s\n",
                    addr, fsr, ip, is_prefetch ? "true" : "false", is_write ? "true" : "false");
        tcb_print_regs(vcpu_id);
        vcpu_print_regs(vcpu_id);
    } else {
        return fault_advance_vcpu(vcpu_id, &regs);
    }

    return success;
}

bool fault_handle(size_t vcpu_id, seL4_MessageInfo_t msginfo)
{
    size_t label = seL4_MessageInfo_get_label(msginfo);
    bool success = false;
    switch (label) {
    case seL4_Fault_VMFault:
        success = fault_handle_vm_exception(vcpu_id);
        break;
    case seL4_Fault_UnknownSyscall:
        success = fault_handle_unknown_syscall(vcpu_id);
        break;
    case seL4_Fault_UserException:
        success = fault_handle_user_exception(vcpu_id);
        break;
    case seL4_Fault_VGICMaintenance:
        success = vgic_handle_fault_maintenance(vcpu_id);
        break;
    case seL4_Fault_VCPUFault:
        success = fault_handle_vcpu_exception(vcpu_id);
        break;
    case seL4_Fault_VPPIEvent:
        success = fault_handle_vppi_event(vcpu_id);
        break;
    default:
        /* We have reached a genuinely unexpected case, stop the guest. */
        LOG_VMM_ERR("unknown fault label 0x%lx, stopping guest with ID 0x%lx\n", label, vcpu_id);
        vmm_vcpu_stop(vcpu_id);
    }

    if (!success) {
        LOG_VMM_ERR("Failed to handle %s fault\n", fault_to_string(label));
        tcb_print_regs(vcpu_id);
        vcpu_print_regs(vcpu_id);
    }

    return success;
}
