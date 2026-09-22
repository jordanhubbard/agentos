/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <libvmm/arch/aarch64/fault.h>

static unsigned calls[3];
static bool device(size_t cpu, size_t offset, size_t fsr,
                   seL4_UserContext *regs, void *context)
{
    assert(cpu == 0 && offset == 7 && fsr == 42 && regs == NULL);
    ++*(unsigned *)context;
    return true;
}

int main(void)
{
    for (unsigned generation = 0; generation < 3; ++generation) {
        assert(!fault_handle_registered_vm_exceptions(0, 0x1007, 42, NULL));
        assert(fault_register_vm_exception_handler(0x1000, 0x100, device, &calls[generation]));
        assert(!fault_register_vm_exception_handler(0x1080, 0x100, device, NULL));
        assert(fault_handle_registered_vm_exceptions(0, 0x1007, 42, NULL));
        assert(calls[generation] == 1);
        assert(!fault_handle_registered_vm_exceptions(0, 0x1100, 42, NULL));
        fault_reset_vm_exception_handlers();
        assert(!fault_handle_registered_vm_exceptions(0, 0x1007, 42, NULL));
        fault_reset_vm_exception_handlers();
        for (unsigned prior = 0; prior <= generation; ++prior) assert(calls[prior] == 1);
    }
    assert(!fault_register_vm_exception_handler(UINTPTR_MAX - 1, 4, device, NULL));
    assert(!fault_register_vm_exception_handler(1, 0, device, NULL));
    assert(!fault_register_vm_exception_handler(1, 1, NULL, NULL));
    for (unsigned i = 0; i < 15; ++i)
        assert(fault_register_vm_exception_handler(0x2000 + i * 0x100, 0x100, device, NULL));
    assert(!fault_register_vm_exception_handler(0x4000, 0x100, device, NULL));
    fault_reset_vm_exception_handlers();
    assert(fault_register_vm_exception_handler(0x4000, 0x100, device, NULL));
    puts("PASS: retired ARM fault handlers cannot dispatch; fresh generations reuse their ranges");
    return 0;
}
