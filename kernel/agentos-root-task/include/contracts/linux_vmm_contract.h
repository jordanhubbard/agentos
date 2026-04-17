/*
 * LinuxVMM IPC Contract
 *
 * The LinuxVMM PD manages Linux guest OS instances.  It implements the
 * agentOS VMM binding protocol (guest_contract.h) and the generic VM
 * lifecycle API (vmm_contract.h).
 *
 * Channel: (assigned in agentos.system; controller → linux_vmm)
 * Opcodes: MSG_VM_* (see agentos.h)
 *
 * Invariants:
 *   - LinuxVMM must complete the guest binding protocol (guest_contract.h §3.1)
 *     before running any guest code.
 *   - LinuxVMM includes contracts/guest_contract.h and contracts/vmm_contract.h.
 *   - LinuxVMM may NOT implement its own device drivers for any device class
 *     that has a generic device PD (serial_pd, net_pd, block_pd, usb_pd).
 *   - MSG_VM_STATUS reflects the guest OS lifecycle state (CREATING → BOOTING
 *     → RUNNING → PAUSED → DEAD).
 *   - MSG_VM_LIST enumerates all Linux guest slots.
 *
 * See vmm_contract.h for the generic VM lifecycle request/reply structs.
 * This header defines Linux-specific extensions only.
 */

#pragma once
#include "../agentos.h"
#include "vmm_contract.h"
#include "guest_contract.h"

/* ─── Linux-specific VM types ────────────────────────────────────────────── */

#define LINUX_VMM_OS_TYPE   0x01u   /* VIBEOS_TYPE_LINUX */

/* ─── Linux-specific create parameters (in vmm_create_req.os_params[]) ───── */

typedef struct __attribute__((packed)) {
    uint32_t kernel_inode;      /* AgentFS inode of bzImage/Image */
    uint32_t initrd_inode;      /* AgentFS inode of initrd (0 = none) */
    uint8_t  cmdline[128];      /* NUL-terminated kernel command line */
} linux_vmm_create_params_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum linux_vmm_error {
    LINUX_VMM_OK              = 0,
    LINUX_VMM_ERR_NO_KERNEL   = 1,  /* kernel_inode not found in AgentFS */
    LINUX_VMM_ERR_NO_RAM      = 2,  /* insufficient RAM for guest */
    LINUX_VMM_ERR_BIND_FAIL   = 3,  /* guest binding protocol failed */
    LINUX_VMM_ERR_SLOT_FULL   = 4,  /* no available VM slots */
};
