/*
 * VMM (Virtual Machine Monitor) IPC Contract
 *
 * Two sections:
 *
 * Section A — External management API (MSG_VM_*, used by callers with SpawnCap):
 *   MSG_VM_CREATE   — create a new guest OS slot (returns vm_id)
 *   MSG_VM_DESTROY  — destroy guest OS slot and release all resources
 *   MSG_VM_SWITCH   — set active console output to this guest
 *   MSG_VM_STATUS   — query guest state (CREATING, BOOTING, RUNNING, etc.)
 *   MSG_VM_LIST     — enumerate all guest slots
 *
 * Section B — VMM-to-root-task internal protocol (MSG_VMM_*, used by VMM PDs):
 *   MSG_VMM_REGISTER        — register this PD as a VMM, receive vmm_token
 *   MSG_VMM_ALLOC_GUEST_MEM — allocate seL4 Untyped frames for guest RAM
 *   MSG_VMM_VCPU_CREATE     — create a vCPU capability for a guest slot
 *   MSG_VMM_VCPU_DESTROY    — destroy a vCPU capability
 *   MSG_VMM_VCPU_SET_REGS   — write guest register state
 *   MSG_VMM_VCPU_GET_REGS   — read guest register state
 *   MSG_VMM_INJECT_IRQ      — inject a virtual IRQ into a guest
 *
 * Channel (Section A): CH_GUEST_PD via vmm dispatcher
 * Channel (Section B): CH_VMM_KERNEL (vmm_pd → root-task, PPC)
 * Opcodes: MSG_VM_* and MSG_VMM_* (see agentos.h)
 *
 * Invariants (Section A):
 *   - MSG_VM_CREATE returns a vm_id; all subsequent calls reference it.
 *   - MSG_VM_DESTROY releases ALL device handles held by the guest.
 *   - A guest in DEAD state may not be restarted; destroy and recreate.
 *   - MSG_VM_SWITCH only affects log drain output routing, not IPC routing.
 *   - MSG_VM_LIST results are placed in vmm_shmem region.
 *
 * Invariants (Section B):
 *   - MSG_VMM_REGISTER must succeed before any other MSG_VMM_* call.
 *   - vmm_token is PD-scoped; each VMM PD gets its own token.
 *   - MSG_VMM_ALLOC_GUEST_MEM returns seL4 capability indices, not VA.
 *   - vCPU caps are valid until MSG_VMM_VCPU_DESTROY or VMM deregisters.
 *   - MSG_VMM_VCPU_SET_REGS / GET_REGS use vcpu_regs_t in shmem.
 */

#pragma once
#include "../agentos.h"

/* ─── VM state constants ─────────────────────────────────────────────────── */

#define VM_STATE_CREATING  0
#define VM_STATE_BOOTING   1
#define VM_STATE_RUNNING   2
#define VM_STATE_PAUSED    3
#define VM_STATE_DEAD      4

/* ─── OS type constants ──────────────────────────────────────────────────── */

#define VMM_OS_TYPE_LINUX    0x01u
#define VMM_OS_TYPE_FREEBSD  0x02u

/* ─── VMM flags ──────────────────────────────────────────────────────────── */

#define VMM_FLAG_SMP         (1u << 0)  /* VMM supports SMP guests */
#define VMM_FLAG_NESTED      (1u << 1)  /* VMM supports nested virtualisation */
#define VMM_FLAG_IOMMU       (1u << 2)  /* VMM has IOMMU pass-through capability */

/* ═══════════════════════════════════════════════════════════════════════════
 * Section A — External management API (MSG_VM_*)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct vmm_req_create {
    uint32_t os_type;           /* VMM_OS_TYPE_* */
    uint32_t ram_mb;
    uint32_t device_flags;      /* GUEST_DEV_FLAG_* (see guest_contract.h) */
    uint8_t  os_params[128];    /* OS-specific params (cast to *_create_params_t) */
};

struct vmm_req_destroy {
    uint32_t vm_id;
};

struct vmm_req_switch {
    uint32_t vm_id;
};

struct vmm_req_status {
    uint32_t vm_id;
};

struct vmm_req_list {
    uint32_t max_entries;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct vmm_reply_create {
    uint32_t ok;
    uint32_t vm_id;             /* 0xFFFFFFFF = invalid / error */
};

struct vmm_reply_destroy {
    uint32_t ok;
};

struct vmm_reply_switch {
    uint32_t ok;
};

struct vmm_reply_status {
    uint32_t ok;
    uint32_t state;             /* VM_STATE_* */
    uint32_t ram_used_mb;
    uint32_t uptime_ticks;
};

struct vmm_reply_list {
    uint32_t ok;
    uint32_t count;             /* entries written to vmm_shmem */
};

/* ─── Shmem layout: VM list entry ────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t vm_id;
    uint32_t os_type;
    uint32_t state;
    uint32_t ram_mb;
    uint32_t ram_used_mb;
    uint32_t uptime_ticks;
} vm_list_entry_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Section B — VMM-to-root-task internal protocol (MSG_VMM_*)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ─── Channel reference ──────────────────────────────────────────────────── */

#define VMM_KERNEL_CH  CH_VMM_KERNEL

/* ─── MSG_VMM_REGISTER ───────────────────────────────────────────────────── */

/*
 * A VMM PD calls this at init to register with the root-task.  On success
 * the root-task assigns a vmm_token scoped to this PD.  All subsequent
 * MSG_VMM_* calls must carry the vmm_token in MR1.
 */

struct vmm_register_req {
    uint32_t os_type;           /* VMM_OS_TYPE_* this VMM will host */
    uint32_t flags;             /* VMM_FLAG_* capabilities this VMM provides */
    uint32_t max_guests;        /* max concurrent guest slots requested */
    uint8_t  name[16];          /* human-readable VMM name */
};

struct vmm_register_reply {
    uint32_t ok;
    uint32_t vmm_token;         /* opaque PD-scoped token; 0 = error */
    uint32_t granted_guests;    /* actual guest slots granted (may be < max_guests) */
};

/* ─── MSG_VMM_ALLOC_GUEST_MEM ────────────────────────────────────────────── */

/*
 * Request seL4 Untyped memory frames for guest RAM.  The root-task carves out
 * the requested size from the system's free untyped pool and returns the
 * capability index range [cap_lo, cap_lo + cap_count).  The VMM maps these
 * frames into the guest's IPA space using seL4_ARM_Page_Map (or arch equiv).
 */

struct vmm_alloc_guest_mem_req {
    uint32_t vmm_token;
    uint32_t guest_id;          /* guest slot that will own this memory */
    uint32_t size_mb;           /* requested RAM size in MiB */
    uint32_t align_bits;        /* minimum alignment (e.g. 21 = 2 MiB huge pages) */
};

struct vmm_alloc_guest_mem_reply {
    uint32_t ok;
    uint32_t cap_lo;            /* first seL4 cap index in the allocation */
    uint32_t cap_count;         /* number of contiguous caps */
    uint32_t actual_size_mb;    /* actual size granted */
};

/* ─── MSG_VMM_VCPU_CREATE ────────────────────────────────────────────────── */

/*
 * Create a vCPU seL4 capability for the given guest slot.  Returns vcpu_cap,
 * an opaque index used in subsequent VCPU operations.  Each guest slot may
 * hold up to VMM_MAX_VCPUS vCPUs; the first one is the bootstrap vCPU.
 */

#define VMM_MAX_VCPUS  4u

struct vmm_vcpu_create_req {
    uint32_t vmm_token;
    uint32_t guest_id;
};

struct vmm_vcpu_create_reply {
    uint32_t ok;
    uint32_t vcpu_cap;          /* opaque vCPU cap index; 0xFFFFFFFF = error */
    uint32_t vcpu_index;        /* 0 = bootstrap vCPU */
};

/* ─── MSG_VMM_VCPU_DESTROY ───────────────────────────────────────────────── */

struct vmm_vcpu_destroy_req {
    uint32_t vmm_token;
    uint32_t vcpu_cap;
};

struct vmm_vcpu_destroy_reply {
    uint32_t ok;
};

/* ─── MSG_VMM_VCPU_SET_REGS / GET_REGS ──────────────────────────────────── */

/*
 * vcpu_regs_t is placed in shmem before MSG_VMM_VCPU_SET_REGS, and populated
 * in shmem by MSG_VMM_VCPU_GET_REGS.  Field names follow AArch64 EL1 naming;
 * on RISC-V the same layout maps to equivalent supervisor CSRs.
 */

typedef struct __attribute__((packed)) {
    uint64_t pc;                /* program counter / sepc */
    uint64_t sp;                /* stack pointer / sp */
    uint64_t x[31];             /* general-purpose registers x1..x31 */
    uint64_t spsr;              /* saved program status / sstatus */
    uint64_t elr;               /* exception link register / sepc alias */
    uint64_t ttbr0;             /* translation table base / satp */
    uint64_t ttbr1;             /* second-stage (VTCR_EL2) / not used on RV */
    uint64_t vbar;              /* vector base / stvec */
    uint32_t _reserved[4];
} vcpu_regs_t;

struct vmm_vcpu_set_regs_req {
    uint32_t vmm_token;
    uint32_t vcpu_cap;
    /* vcpu_regs_t is in shmem */
};

struct vmm_vcpu_set_regs_reply {
    uint32_t ok;
};

struct vmm_vcpu_get_regs_req {
    uint32_t vmm_token;
    uint32_t vcpu_cap;
};

struct vmm_vcpu_get_regs_reply {
    uint32_t ok;
    /* vcpu_regs_t written to shmem on success */
};

/* ─── MSG_VMM_INJECT_IRQ ─────────────────────────────────────────────────── */

/*
 * Inject a virtual IRQ into a guest vCPU.  Used by device PDs to deliver
 * virtio interrupt notifications without polling.  irq_num is the guest-side
 * virtual interrupt number (e.g. GIC INTID or RISC-V PLIC source).
 */

struct vmm_inject_irq_req {
    uint32_t vmm_token;
    uint32_t guest_id;
    uint32_t vcpu_cap;          /* target vCPU; 0 = bootstrap vCPU */
    uint32_t irq_num;           /* virtual interrupt number */
    uint32_t level;             /* 1 = assert, 0 = deassert */
};

struct vmm_inject_irq_reply {
    uint32_t ok;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum vmm_error {
    VMM_OK                   = 0,
    VMM_ERR_NO_SLOTS         = 1,   /* all VM slots occupied */
    VMM_ERR_BAD_VM_ID        = 2,
    VMM_ERR_BAD_OS_TYPE      = 3,
    VMM_ERR_DEAD             = 4,   /* operation on DEAD guest */
    VMM_ERR_BIND_FAIL        = 5,   /* guest binding protocol failed */
    VMM_ERR_NOT_REGISTERED   = 6,   /* MSG_VMM_* without prior REGISTER */
    VMM_ERR_BAD_TOKEN        = 7,   /* vmm_token unknown or revoked */
    VMM_ERR_NO_MEM           = 8,   /* insufficient free untyped memory */
    VMM_ERR_MAX_VCPUS        = 9,   /* VMM_MAX_VCPUS already created */
    VMM_ERR_BAD_VCPU_CAP     = 10,  /* vcpu_cap unknown or already destroyed */
    VMM_ERR_BAD_ALIGN        = 11,  /* align_bits not a valid page order */
    VMM_ERR_IRQ_RANGE        = 12,  /* irq_num out of range for this guest */
};
