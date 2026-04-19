#pragma once
/* LINUX_VMM contract — version 1
 * PD: linux_vmm | Source: src/linux_vmm.c | Channel: CH_VM_MANAGER=45 (from controller, via vm_manager)
 */
#include <stdint.h>
#include <stdbool.h>

#define LINUX_VMM_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
/* linux_vmm is managed via vm_manager; CH_VM_MANAGER=45 dispatches to linux_vmm slots */
#define CH_VM_MANAGER              45u  /* cross-ref: agentos.h */

/* ── Opcodes (shared with vm_manager) ── */
#define LINUX_VMM_OP_CREATE        0x10u  /* create a new Linux VM instance */
#define LINUX_VMM_OP_DESTROY       0x11u  /* destroy a Linux VM and reclaim resources */
#define LINUX_VMM_OP_START         0x12u  /* start (boot) a created VM */
#define LINUX_VMM_OP_STOP          0x13u  /* stop a running VM (ACPI shutdown) */
#define LINUX_VMM_OP_PAUSE         0x14u  /* pause vCPU execution */
#define LINUX_VMM_OP_RESUME        0x15u  /* resume paused vCPU execution */
#define LINUX_VMM_OP_CONSOLE       0x16u  /* attach/detach serial console ring */
#define LINUX_VMM_OP_INFO          0x17u  /* query VM state and resource usage */
#define LINUX_VMM_OP_LIST          0x18u  /* list all linux_vmm instances */
#define LINUX_VMM_OP_SNAPSHOT      0x19u  /* checkpoint VM state to AgentFS */
#define LINUX_VMM_OP_RESTORE       0x1Au  /* restore VM from AgentFS snapshot */

/* Linux-VMM-specific extended opcodes */
#define LINUX_VMM_OP_SET_CMDLINE   0x1Bu  /* set kernel command line before start */
#define LINUX_VMM_OP_INJECT_IRQ    0x1Cu  /* inject virtual IRQ into VM */
#define LINUX_VMM_OP_MMIO_MAP      0x1Du  /* map MMIO region into VM guest physical */

/* ── VM states ── */
#define LINUX_VMM_STATE_CREATED    0u  /* allocated, not yet started */
#define LINUX_VMM_STATE_RUNNING    1u  /* vCPUs active */
#define LINUX_VMM_STATE_PAUSED     2u  /* vCPUs halted, state preserved */
#define LINUX_VMM_STATE_STOPPING   3u  /* ACPI shutdown in progress */
#define LINUX_VMM_STATE_STOPPED    4u  /* stopped, resources held */
#define LINUX_VMM_STATE_SNAPSHOT   5u  /* snapshot in progress */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* LINUX_VMM_OP_CREATE */
    uint64_t kernel_hash_lo;  /* AgentFS hash of kernel Image */
    uint64_t kernel_hash_hi;
    uint64_t dtb_hash_lo;     /* AgentFS hash of device tree blob (0 = use default) */
    uint64_t dtb_hash_hi;
    uint32_t ram_mb;          /* guest physical RAM in MiB */
    uint32_t vcpu_count;      /* number of vCPUs (1-8) */
    uint32_t flags;           /* LINUX_VMM_FLAG_* */
} linux_vmm_req_create_t;

#define LINUX_VMM_FLAG_VIRTIO_NET  (1u << 0)  /* attach virtio-net device */
#define LINUX_VMM_FLAG_VIRTIO_BLK  (1u << 1)  /* attach virtio-blk device */
#define LINUX_VMM_FLAG_SERIAL      (1u << 2)  /* attach bidirectional serial console */

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else linux_vmm_error_t */
    uint32_t slot_id;         /* VM slot handle for subsequent operations */
} linux_vmm_reply_create_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* LINUX_VMM_OP_DESTROY */
    uint32_t slot_id;
} linux_vmm_req_destroy_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} linux_vmm_reply_destroy_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* LINUX_VMM_OP_START */
    uint32_t slot_id;
} linux_vmm_req_start_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} linux_vmm_reply_start_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* LINUX_VMM_OP_INFO */
    uint32_t slot_id;
} linux_vmm_req_info_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t state;           /* LINUX_VMM_STATE_* */
    uint32_t ram_mb;
    uint32_t vcpu_count;
    uint64_t uptime_ns;       /* VM uptime in nanoseconds */
    uint64_t ram_vaddr;       /* host virtual address of guest RAM region */
} linux_vmm_reply_info_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* LINUX_VMM_OP_SNAPSHOT */
    uint32_t slot_id;
} linux_vmm_req_snapshot_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint64_t snap_hash_lo;    /* AgentFS hash of snapshot object */
    uint64_t snap_hash_hi;
} linux_vmm_reply_snapshot_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* LINUX_VMM_OP_RESTORE */
    uint32_t slot_id;
    uint64_t snap_hash_lo;
    uint64_t snap_hash_hi;
} linux_vmm_req_restore_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} linux_vmm_reply_restore_t;

/* ── Error codes ── */
typedef enum {
    LINUX_VMM_OK              = 0,
    LINUX_VMM_ERR_NO_SLOT     = 1,  /* no free VM slot available */
    LINUX_VMM_ERR_BAD_SLOT    = 2,  /* slot_id invalid */
    LINUX_VMM_ERR_BAD_STATE   = 3,  /* operation not valid in current VM state */
    LINUX_VMM_ERR_NO_KERNEL   = 4,  /* kernel hash not found in AgentFS */
    LINUX_VMM_ERR_NO_MEM      = 5,  /* insufficient physical memory for ram_mb */
    LINUX_VMM_ERR_VCPU_FAULT  = 6,  /* unhandled vCPU fault */
    LINUX_VMM_ERR_SNAP_FAIL   = 7,  /* snapshot serialization failed */
    LINUX_VMM_ERR_RESTORE_FAIL= 8,  /* snapshot restore failed (corrupt/version mismatch) */
} linux_vmm_error_t;

/* ── Invariants ──
 * - linux_vmm runs in Ring 3; it has no direct hardware access.
 * - Guest RAM is allocated from capabilities granted by the root task.
 * - SNAPSHOT pauses the VM; RESTORE requires the target slot to be in STOPPED state.
 * - Serial console ring uses the log_drain_ring_t layout (LOG_DRAIN_RING_MAGIC).
 * - vcpu_count must be between 1 and 8 inclusive.
 * - Destroying a running VM forces STOP first; all capabilities are reclaimed.
 */
