/*
 * vmm_mux_stub.c — no-op VM multiplexer stubs for non-AArch64 targets
 *
 * libvmm (and the real vmm_mux.c) is only available on AArch64 with a
 * FreeBSD guest.  On RISC-V / x86 builds vm_manager.c still compiles and
 * handles IPC; every vmm_mux_* call gracefully returns a "not supported"
 * result so the rest of the system is unaffected.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "agentos.h"
#include "sel4_server.h"
#include "vm_manager.h"

/* Required by vmm_mux.h extern declarations */
uintptr_t guest_ram_vaddr_0;
uintptr_t guest_ram_vaddr_1;
uintptr_t guest_ram_vaddr_2;
uintptr_t guest_ram_vaddr_3;
uintptr_t guest_flash_vaddr;

void vmm_mux_init(vm_mux_t *mux)
{
    for (int i = 0; i < VM_MAX_SLOTS; i++)
        mux->slots[i].state = VM_SLOT_FREE;
    mux->active_slot = 0;
    mux->slot_count  = 0;
}

uint8_t vmm_mux_create(vm_mux_t *mux __attribute__((unused)),
                        const char *label __attribute__((unused)))
{
    return 0xFF; /* no free slots / not supported */
}

int vmm_mux_destroy(vm_mux_t *mux __attribute__((unused)),
                    uint8_t slot_id __attribute__((unused)))
{
    return -1;
}

int vmm_mux_pause(vm_mux_t *mux __attribute__((unused)),
                  uint8_t slot_id __attribute__((unused)))
{
    return -1;
}

int vmm_mux_resume(vm_mux_t *mux __attribute__((unused)),
                   uint8_t slot_id __attribute__((unused)))
{
    return -1;
}

int vmm_mux_switch(vm_mux_t *mux __attribute__((unused)),
                   uint8_t slot_id __attribute__((unused)))
{
    return -1;
}

void vmm_mux_handle_fault(vm_mux_t *mux __attribute__((unused)),
                           seL4_CPtr child __attribute__((unused)),
                           uint32_t msginfo __attribute__((unused)),
                           uint32_t *reply_msginfo __attribute__((unused)))
{
}

void vmm_mux_handle_notify(vm_mux_t *mux __attribute__((unused)),
                            uint32_t ch __attribute__((unused)))
{
}

void vmm_mux_status(const vm_mux_t *mux __attribute__((unused)),
                    uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) out[i] = VM_SLOT_FREE;
}
