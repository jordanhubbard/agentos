/*
 * smc_stub.c — no-op replacement for libvmm/src/arch/aarch64/smc.c
 *
 * seL4_ARM_SMC is a typedef (not a function) in Microkit SDK 2.1, so the
 * real smc.c fails to compile.  On QEMU virt, PSCI is handled by QEMU's
 * built-in emulation; the seL4 VMM never needs to forward SMC calls to
 * actual EL3 firmware.  All SMC-related functions become no-ops.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sel4/sel4.h>
#include <libvmm/arch/aarch64/smc.h>

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
    (void)vcpu_id;
    (void)hsr;
    return false;
}
