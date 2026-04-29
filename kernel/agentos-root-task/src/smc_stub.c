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

#define SMCCC_RET_SUCCESS       0u
#define SMCCC_RET_NOT_SUPPORTED ((uint64_t)-1)
#define SMCCC_TRNG_VERSION      0x50u
#define SMCCC_TRNG_FEATURES     0x51u
#define SMCCC_TRNG_GET_UUID     0x52u
#define SMCCC_TRNG_RND32        0x53u
#define SMCCC_TRNG_RND64        0x53u
#define SMCCC_TRNG_VERSION_1_0  0x00010000u

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

static uint64_t smc_trng_next(void)
{
    static uint64_t state = 0x41474f5354524e47ull; /* "AGOSTRNG" */
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static bool smc_handle_trng(size_t vcpu_id, seL4_UserContext *regs)
{
    uint64_t fn = regs->x0 & SMC_FUNC_ID_MASK;

    switch (fn) {
    case SMCCC_TRNG_VERSION:
        regs->x0 = SMCCC_TRNG_VERSION_1_0;
        break;
    case SMCCC_TRNG_FEATURES:
        switch (regs->x1 & SMC_FUNC_ID_MASK) {
        case SMCCC_TRNG_VERSION:
        case SMCCC_TRNG_FEATURES:
        case SMCCC_TRNG_GET_UUID:
        case SMCCC_TRNG_RND32:
            regs->x0 = SMCCC_RET_SUCCESS;
            break;
        default:
            regs->x0 = SMCCC_RET_NOT_SUPPORTED;
            break;
        }
        break;
    case SMCCC_TRNG_GET_UUID:
        regs->x0 = 0x3cc0d623u;
        regs->x1 = 0x1d5c4fbeu;
        regs->x2 = 0x8a7a3f19u;
        regs->x3 = 0x41474f53u;
        break;
    case SMCCC_TRNG_RND64:
        regs->x0 = SMCCC_RET_SUCCESS;
        regs->x1 = smc_trng_next();
        regs->x2 = smc_trng_next();
        regs->x3 = smc_trng_next();
        break;
    default:
        regs->x0 = SMCCC_RET_NOT_SUPPORTED;
        break;
    }

    extern bool fault_advance_vcpu(size_t vcpu_id, seL4_UserContext *regs);
    return fault_advance_vcpu(vcpu_id, regs);
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
    if (service == SMC_CALL_STD_SERVICE &&
        (fn_number == SMCCC_TRNG_VERSION ||
         fn_number == SMCCC_TRNG_FEATURES ||
         fn_number == SMCCC_TRNG_GET_UUID ||
         fn_number == SMCCC_TRNG_RND32)) {
        return smc_handle_trng(vcpu_id, &regs);
    }

    /* SMCCC architectural calls (ARM DEN0028):
     *   fn 0 = SMCCC_VERSION  → return v1.1 (0x00010001)
     *   fn 1 = SMCCC_ARCH_FEATURES → NOT_SUPPORTED (-1)
     *   others → NOT_SUPPORTED */
    if (service == SMC_CALL_ARM_ARCH) {
        smc_set_return_value(&regs, fn_number == 0u ? 0x00010001u : SMCCC_RET_NOT_SUPPORTED);
        extern bool fault_advance_vcpu(size_t vcpu_id, seL4_UserContext *regs);
        return fault_advance_vcpu(vcpu_id, &regs);
    }

    /* All other SMC calls (standard service extensions, SiP, OEM, Trusted OS)
     * are not supported in our software-emulated VMM.  Return NOT_SUPPORTED so
     * the guest can detect and fall back rather than hanging. */
    LOG_VMM("Unhandled SMC ignored: service=0x%lx fn=0x%lx (x0=0x%lx) → NOT_SUPPORTED\n",
            service, fn_number, regs.x0);
    smc_set_return_value(&regs, SMCCC_RET_NOT_SUPPORTED);
    extern bool fault_advance_vcpu(size_t vcpu_id, seL4_UserContext *regs);
    return fault_advance_vcpu(vcpu_id, &regs);
}
