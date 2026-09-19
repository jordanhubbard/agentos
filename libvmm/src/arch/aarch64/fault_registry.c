/* Copyright 2017, Data61, CSIRO (ABN 57 195 873 179)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 * SPDX-License-Identifier: BSD-2-Clause */
#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/util/util.h>

#define MAX_VM_EXCEPTION_HANDLERS 16u
static struct {
    uintptr_t base, end;
    vm_exception_handler_t callback;
    void *data;
} handlers[MAX_VM_EXCEPTION_HANDLERS];
static size_t count;

void fault_reset_vm_exception_handlers(void)
{
    /* Caller owns a stopped guest and excludes dispatch throughout reset. */
    for (size_t i = 0; i < count; ++i) {
        handlers[i].base = handlers[i].end = 0;
        handlers[i].callback = NULL;
        handlers[i].data = NULL;
    }
    count = 0;
}

bool fault_register_vm_exception_handler(uintptr_t base, size_t size,
                                        vm_exception_handler_t callback, void *data)
{
    if (count >= MAX_VM_EXCEPTION_HANDLERS - 1 || !size || !callback ||
        size > UINTPTR_MAX - base) {
        LOG_VMM_ERR("invalid or exhausted VM exception handler registration\n");
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (base < handlers[i].end && base + size > handlers[i].base) {
            LOG_VMM_ERR("VM exception handler [0x%lx..0x%lx) overlaps [0x%lx..0x%lx)\n",
                        base, base + size, handlers[i].base, handlers[i].end);
            return false;
        }
    }
    handlers[count].base = base;
    handlers[count].end = base + size;
    handlers[count].callback = callback;
    handlers[count].data = data;
    ++count;
    return true;
}

bool fault_handle_registered_vm_exceptions(size_t vcpu_id, uintptr_t addr,
                                          size_t fsr, seL4_UserContext *regs)
{
    for (size_t i = 0; i < count; ++i) {
        if (addr >= handlers[i].base && addr < handlers[i].end) {
            bool ok = handlers[i].callback(vcpu_id, addr - handlers[i].base,
                                           fsr, regs, handlers[i].data);
            if (!ok) LOG_VMM_ERR("registered VM exception handler at 0x%lx failed\n", addr);
            return ok;
        }
    }
    return false;
}
