/*
 * FreeBSDVMM IPC Contract
 *
 * The FreeBSDVMM PD manages FreeBSD guest OS instances.  It implements the
 * agentOS VMM binding protocol (guest_contract.h) and the generic VM
 * lifecycle API (vmm_contract.h).
 *
 * Channel: (assigned in agentos.system; controller → freebsd_vmm)
 * Opcodes: MSG_VM_* / OP_VM_* (see agentos.h)
 *
 * Invariants:
 *   - FreeBSDVMM must complete the guest binding protocol (guest_contract.h §3.1)
 *     before running any guest code.
 *   - FreeBSDVMM includes contracts/guest_contract.h and contracts/vmm_contract.h.
 *   - FreeBSDVMM may NOT implement its own device drivers for any device class
 *     that has a generic device PD (serial_pd, net_pd, block_pd, usb_pd).
 *   - FreeBSD loaders and ELF images are fetched from AgentFS.
 *
 * See vmm_contract.h for the generic VM lifecycle request/reply structs.
 * This header defines FreeBSD-specific extensions only.
 */

#pragma once
#include "../agentos.h"
#include "vmm_contract.h"
#include "guest_contract.h"

/* ─── FreeBSD-specific VM types ──────────────────────────────────────────── */

#define FREEBSD_VMM_OS_TYPE  0x02u  /* VIBEOS_TYPE_FREEBSD */

/* ─── FreeBSD-specific create parameters ─────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t loader_inode;      /* AgentFS inode of ubldr or loader.efi */
    uint32_t rootfs_inode;      /* AgentFS inode of rootfs image */
    uint8_t  boot_args[128];    /* NUL-terminated boot arguments */
} freebsd_vmm_create_params_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum freebsd_vmm_error {
    FREEBSD_VMM_OK              = 0,
    FREEBSD_VMM_ERR_NO_LOADER   = 1,
    FREEBSD_VMM_ERR_NO_RAM      = 2,
    FREEBSD_VMM_ERR_BIND_FAIL   = 3,
    FREEBSD_VMM_ERR_SLOT_FULL   = 4,
};
