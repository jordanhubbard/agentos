/*
 * pd_entry.c — AArch64/RISC-V/x86-64 PD entry point for agentOS service PDs
 *
 * Provides the _start symbol that the linker uses as the ELF entry point
 * (see tools/ld/agentos.ld ENTRY(_start)).  This file is compiled into
 * every service PD but NOT into the root task (which has its own _start in
 * start_aarch64.S / main.c).
 *
 * _start(my_ep, ns_ep) is called by the seL4 kernel via the TCB's initial PC
 * with arguments placed in x0/x1 (AArch64) or a0/a1 (RISC-V) by the root
 * task's pd_tcb_set_regs call.
 *
 *   my_ep   — CNode slot of this PD's own server listen endpoint
 *   ns_ep   — CNode slot of the nameserver endpoint (PD_CNODE_SLOT_NAMESERVER_EP)
 *
 * Before calling pd_main, _start fixes the IPC buffer pointer to the
 * seL4-mapped frame at PD_IPC_BUF_VA (0x10000000) so that seL4 IPC calls
 * use the correct shared page rather than the BSS placeholder set up by
 * sel4_crt.c's static initializer.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stddef.h>
#include <stdint.h>

/*
 * seL4 IPC buffer pointer — defined in sel4_crt.c.
 * We reassign it here to the seL4-mapped page before any IPC call.
 */
typedef struct seL4_IPCBuffer_ seL4_IPCBuffer;
extern seL4_IPCBuffer *__sel4_ipc_buffer;

/* Virtual address where the root task maps each PD's IPC buffer frame. */
#define PD_IPC_BUF_VA  0x10000000UL

/*
 * Every service PD must define pd_main(seL4_CPtr ep, seL4_CPtr ns_ep).
 * Declared weak so that a missing pd_main produces a link-time NULL rather
 * than a linker error; _start guards against the NULL before calling it.
 */
typedef unsigned long seL4_CPtr;
extern __attribute__((weak)) void pd_main(seL4_CPtr ep, seL4_CPtr ns_ep);

/*
 * _start — ELF entry point for service PDs.
 *
 * Placed in .text.start so the linker script (agentos.ld) can guarantee it
 * is the very first instruction at 0x400000, matching the ELF e_entry.
 */
__attribute__((section(".text.start"), noreturn, used))
void _start(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    /* Redirect __sel4_ipc_buffer to the seL4-mapped page. */
    __sel4_ipc_buffer = (seL4_IPCBuffer *)(uintptr_t)PD_IPC_BUF_VA;

    /* Call the PD's entry function if defined. */
    if (pd_main) {
        pd_main(my_ep, ns_ep);
    }

    /* Should never reach here; spin to prevent undefined behaviour. */
    for (;;) {
        /* yield if we have a yield syscall — otherwise busy-wait */
        __asm__ volatile("" ::: "memory");
    }
}
