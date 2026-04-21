#pragma once
/* VM_MANAGER contract — version 1
 * PD: vm_manager | Source: src/vm_manager.c | Channel: CH_VM_MANAGER=45 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define VM_MANAGER_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_VM_MANAGER              45u  /* controller -> vm_manager (PPC); cross-ref: agentos.h */
#define CH_VM_SNAPSHOT             46u  /* controller -> vm_snapshot PD (Phase 2) */

/* ── Opcodes ── */
#define VM_MANAGER_OP_CREATE       0x10u  /* create a new guest VM instance */
#define VM_MANAGER_OP_DESTROY      0x11u  /* destroy a VM and reclaim all capabilities */
#define VM_MANAGER_OP_START        0x12u  /* start (boot) a created VM */
#define VM_MANAGER_OP_STOP         0x13u  /* stop a running VM (ACPI shutdown) */
#define VM_MANAGER_OP_PAUSE        0x14u  /* pause vCPU execution */
#define VM_MANAGER_OP_RESUME       0x15u  /* resume paused vCPU execution */
#define VM_MANAGER_OP_CONSOLE      0x16u  /* attach/detach serial console ring */
#define VM_MANAGER_OP_INFO         0x17u  /* query VM state, resource usage */
#define VM_MANAGER_OP_LIST         0x18u  /* list all VM instances */
#define VM_MANAGER_OP_SNAPSHOT     0x19u  /* checkpoint VM state to AgentFS */
#define VM_MANAGER_OP_RESTORE      0x1Au  /* restore VM from AgentFS snapshot */

/* vibeOS-parity extended operations */
#define VM_MANAGER_OP_ATTACH       0x1Bu  /* attach a generic device service to a VM */
#define VM_MANAGER_OP_DETACH       0x1Cu  /* detach a generic device service from a VM */
#define VM_MANAGER_OP_MIGRATE      0x1Du  /* move VM to another capability domain */
#define VM_MANAGER_OP_CONFIGURE    0x1Eu  /* modify VM parameters without destroying */

/* ── VM types ── */
#define VM_TYPE_LINUX              0u  /* Linux guest (linux_vmm) */
#define VM_TYPE_FREEBSD            1u  /* FreeBSD guest (freebsd_vmm) */

/* ── VM states ── */
#define VM_STATE_CREATED           0u  /* resources allocated, not yet started */
#define VM_STATE_STARTING          1u  /* boot in progress */
#define VM_STATE_RUNNING           2u  /* vCPUs active */
#define VM_STATE_PAUSED            3u  /* vCPUs halted, state preserved */
#define VM_STATE_STOPPING          4u  /* ACPI shutdown in progress */
#define VM_STATE_STOPPED           5u  /* stopped, resources held */
#define VM_STATE_SNAPSHOTTING      6u  /* checkpoint operation in progress */
#define VM_STATE_RESTORING         7u  /* restore operation in progress */
#define VM_STATE_MIGRATING         8u  /* live migration in progress */
#define VM_STATE_ERROR             9u  /* VM encountered unrecoverable error */

/* ── Device service types for ATTACH/DETACH ── */
#define VM_DEV_SERIAL              0u  /* serial-mux (canonical UART service) */
#define VM_DEV_NET                 1u  /* net-service (virtio-net backend) */
#define VM_DEV_BLOCK               2u  /* block-service (virtio-blk backend) */
#define VM_DEV_RNG                 3u  /* entropy-service */
#define VM_DEV_TIMER               4u  /* timer-service */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VM_MANAGER_OP_CREATE */
    uint32_t vm_type;         /* VM_TYPE_* */
    uint64_t kernel_hash_lo;  /* AgentFS hash of kernel image */
    uint64_t kernel_hash_hi;
    uint64_t initrd_hash_lo;  /* AgentFS hash of initrd (0 = none) */
    uint64_t initrd_hash_hi;
    uint32_t ram_mb;          /* guest physical RAM in MiB */
    uint32_t vcpu_count;      /* vCPU count (1-8) */
    uint32_t flags;           /* VM_CREATE_FLAG_* */
} vm_manager_req_create_t;

#define VM_CREATE_FLAG_VIRTIO_NET  (1u << 0)
#define VM_CREATE_FLAG_VIRTIO_BLK  (1u << 1)
#define VM_CREATE_FLAG_SERIAL      (1u << 2)
#define VM_CREATE_FLAG_RNG         (1u << 3)

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else vm_manager_error_t */
    uint32_t slot_id;         /* VM slot handle */
} vm_manager_reply_create_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VM_MANAGER_OP_DESTROY */
    uint32_t slot_id;
} vm_manager_req_destroy_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} vm_manager_reply_destroy_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VM_MANAGER_OP_START */
    uint32_t slot_id;
} vm_manager_req_start_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} vm_manager_reply_start_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VM_MANAGER_OP_STOP */
    uint32_t slot_id;
    uint32_t force;           /* 1 = hard stop (no ACPI); 0 = graceful */
    uint32_t timeout_ms;      /* timeout for graceful stop before forcing */
} vm_manager_req_stop_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} vm_manager_reply_stop_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VM_MANAGER_OP_INFO */
    uint32_t slot_id;
} vm_manager_req_info_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t state;           /* VM_STATE_* */
    uint32_t vm_type;         /* VM_TYPE_* */
    uint32_t ram_mb;
    uint32_t vcpu_count;
    uint64_t uptime_ns;
    uint64_t ram_vaddr;       /* host VA of guest RAM base */
} vm_manager_reply_info_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VM_MANAGER_OP_LIST */
    uint32_t shmem_offset;
    uint32_t max_entries;
} vm_manager_req_list_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t entry_count;
    uint32_t total_vms;
} vm_manager_reply_list_t;

/* Entry written to shmem for LIST */
typedef struct __attribute__((packed)) {
    uint32_t slot_id;
    uint32_t vm_type;
    uint32_t state;
    uint32_t ram_mb;
    uint32_t vcpu_count;
    uint64_t uptime_ns;
} vm_manager_list_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VM_MANAGER_OP_SNAPSHOT */
    uint32_t slot_id;
} vm_manager_req_snapshot_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint64_t snap_hash_lo;
    uint64_t snap_hash_hi;
} vm_manager_reply_snapshot_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VM_MANAGER_OP_RESTORE */
    uint32_t slot_id;
    uint64_t snap_hash_lo;
    uint64_t snap_hash_hi;
} vm_manager_req_restore_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} vm_manager_reply_restore_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VM_MANAGER_OP_ATTACH */
    uint32_t slot_id;
    uint32_t dev_type;        /* VM_DEV_* */
    uint32_t service_ch;      /* controller channel of the service PD to bind */
} vm_manager_req_attach_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} vm_manager_reply_attach_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VM_MANAGER_OP_CONFIGURE */
    uint32_t slot_id;
    uint32_t new_vcpu_count;  /* 0 = no change */
    uint32_t new_ram_mb;      /* 0 = no change */
    uint32_t flags;           /* VM_CREATE_FLAG_* bitmask to update */
} vm_manager_req_configure_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} vm_manager_reply_configure_t;

/* ── Error codes ── */
typedef enum {
    VM_MANAGER_OK             = 0,
    VM_MANAGER_ERR_NO_SLOT    = 1,  /* no free VM slot available */
    VM_MANAGER_ERR_BAD_SLOT   = 2,  /* slot_id invalid */
    VM_MANAGER_ERR_BAD_STATE  = 3,  /* operation not valid in current VM state */
    VM_MANAGER_ERR_NO_KERNEL  = 4,  /* kernel hash not found in AgentFS */
    VM_MANAGER_ERR_NO_MEM     = 5,  /* insufficient physical memory */
    VM_MANAGER_ERR_BAD_TYPE   = 6,  /* vm_type not supported */
    VM_MANAGER_ERR_SNAP_FAIL  = 7,  /* snapshot failed */
    VM_MANAGER_ERR_REST_FAIL  = 8,  /* restore failed (corrupt or version mismatch) */
    VM_MANAGER_ERR_DEV_FAIL   = 9,  /* device attach/detach failed */
    VM_MANAGER_ERR_MIG_FAIL   = 10, /* migration failed */
} vm_manager_error_t;

/* ── Invariants ──
 * - vm_manager dispatches to linux_vmm or freebsd_vmm based on VM_TYPE_*.
 * - All VM operations are serialized per slot; concurrent ops on the same slot are rejected.
 * - SNAPSHOT pauses the VM during serialization; VM resumes after AgentFS write completes.
 * - RESTORE requires slot in STOPPED state; it replaces all state.
 * - DESTROY reclaims all seL4 capabilities associated with the VM unconditionally.
 * - vcpu_count must be 1-8; ram_mb must be a multiple of 4 and at least 64.
 * - CONFIGURE without change fields (0 values) is a no-op and returns VM_MANAGER_OK.
 */
