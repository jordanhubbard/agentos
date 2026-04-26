/*
 * smc_stub.c — replacement for libvmm/src/arch/aarch64/smc.c
 *
 * seL4_ARM_SMC is a typedef (not a function) in Microkit SDK 2.1, so the
 * real smc.c fails to compile.  On QEMU virt, PSCI is handled in software
 * by libvmm's psci.c — no seL4_ARM_SMC forwarding needed.
 *
 * Unrecognised SMC calls return NOT_SUPPORTED (-1 in x0) so the guest can
 * probe and continue, rather than the VMM crashing with an unhandled fault.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sel4/sel4.h>
#include <libvmm/arch/aarch64/smc.h>
#include <libvmm/arch/aarch64/psci.h>
#include <libvmm/vcpu.h>
#include <libvmm/util/util.h>

/* SMC calling convention: service type in bits [30:24] of function ID. */
#define SMC_SERVICE_CALL_SHIFT  24u
#define SMC_SERVICE_CALL_MASK   0x3Fu
#define SMC_FUNC_ID_MASK        0xFFFFu
#define SMC_CALL_ARM_ARCH       0u   /* ARM architecture calls: SMCCC_VERSION etc. */
#define SMC_CALL_STD_SERVICE    4u   /* Standard service: PSCI lives here */

void smc_set_return_value(seL4_UserContext *u, uint64_t val)
{
    u->x0 = val;
}

uint64_t smc_get_arg(seL4_UserContext *u, uint64_t arg)
{
    switch (arg) {
    case 1: return u->x1;
    case 2: return u->x2;
    case 3: return u->x3;
    case 4: return u->x4;
    case 5: return u->x5;
    case 6: return u->x6;
    default: return 0;
    }
}

bool smc_register_sip_handler(smc_sip_handler_t handler)
{
    (void)handler;
    return true;
}

bool smc_handle(size_t vcpu_id, uint64_t hsr)
{
    seL4_UserContext regs;
    seL4_TCB_ReadRegisters(vmm_tcb_cap(vcpu_id), false, 0,
                           SEL4_USER_CONTEXT_SIZE, &regs);

    uint64_t service    = (regs.x0 >> SMC_SERVICE_CALL_SHIFT) & SMC_SERVICE_CALL_MASK;
    uint64_t fn_number  = regs.x0 & SMC_FUNC_ID_MASK;

    if (service == SMC_CALL_STD_SERVICE && fn_number < (uint64_t)PSCI_MAX) {
        return handle_psci(vcpu_id, &regs, fn_number, hsr);
    }

    /* SMCCC architectural calls (ARM DEN0028):
     *   fn 0 = SMCCC_VERSION  → return v1.1 (0x00010001)
     *   fn 1 = SMCCC_ARCH_FEATURES → NOT_SUPPORTED (-1)
     *   others → NOT_SUPPORTED */
    if (service == SMC_CALL_ARM_ARCH) {
        smc_set_return_value(&regs, fn_number == 0u ? 0x00010001u : (uint64_t)-1);
        extern bool fault_advance_vcpu(size_t vcpu_id, seL4_UserContext *regs);
        return fault_advance_vcpu(vcpu_id, &regs);
    }

    /* All other SMC calls (standard service extensions, SiP, OEM, Trusted OS)
     * are not supported in our software-emulated VMM.  Return NOT_SUPPORTED so
     * the guest can detect and fall back rather than hanging. */
    LOG_VMM("Unhandled SMC ignored: service=0x%lx fn=0x%lx (x0=0x%lx) → NOT_SUPPORTED\n",
            service, fn_number, regs.x0);
    smc_set_return_value(&regs, (uint64_t)-1);
    extern bool fault_advance_vcpu(size_t vcpu_id, seL4_UserContext *regs);
    return fault_advance_vcpu(vcpu_id, &regs);
}
