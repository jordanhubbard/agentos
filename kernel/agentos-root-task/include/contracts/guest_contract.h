/*
 * Guest OS IPC Contract
 *
 * Defines the formal interface between agentOS and any guest OS running as
 * a VMM Protection Domain.  This contract governs how guest OSes discover,
 * bind to, and use generic device PDs.
 *
 * All VMM PDs (linux_vmm, freebsd_vmm, future VMMs) must:
 *   1. Include this header.
 *   2. Complete the lifecycle protocol below before running guest code.
 *   3. Expose a VirtIO transport backed by the generic device PDs.
 *   4. Not implement any device class for which a generic device PD exists.
 *
 * Two-layer protocol:
 *   Layer A — MSG_GUEST_* lifecycle API (used by external callers with SpawnCap):
 *     CREATE → BIND_DEVICE (one per device) → SET_MEMORY → BOOT
 *     SUSPEND / RESUME during operation
 *     DESTROY on teardown
 *
 *   Layer B — Internal binding types (used by VMM PDs during self-registration):
 *     guest_bind_req, guest_capabilities_t, guest_resource_limits_t
 *     Binding Protocol (PLAN.md §3.1):
 *       1. Subscribe to EventBus for EVENT_GUEST_READY.
 *       2. Query AgentFS /devices namespace for available device PD endpoints.
 *       3. Send MSG_<DEVICE>_OPEN to each required device PD (badge-validated).
 *       4. Send MSG_QUOTA_REGISTER to QuotaPD with resource requirements.
 *       5. Publish MSG_EVENTBUS_PUBLISH_BATCH with EVENT_GUEST_READY.
 *
 * Channel: CH_GUEST_PD (see agentos.h)
 * Opcodes: MSG_GUEST_* (see agentos.h)
 *
 * Invariants:
 *   - MSG_GUEST_CREATE returns a guest_id; all subsequent calls reference it.
 *   - MSG_GUEST_BIND_DEVICE must be called for each required device before BOOT.
 *   - MSG_GUEST_SET_MEMORY must be called exactly once before BOOT.
 *   - MSG_GUEST_BOOT is irreversible; only SUSPEND/RESUME/DESTROY are valid after.
 *   - MSG_GUEST_DESTROY releases ALL capability tokens and device handles.
 *   - A guest in GUEST_STATE_DEAD may not be resumed; destroy and recreate.
 *   - A guest OS may not submit IPC to a device PD without a valid cap_token.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel reference ──────────────────────────────────────────────────── */

#define GUEST_PD_CH_CONTROLLER  CH_GUEST_PD

/* ─── Capability token ───────────────────────────────────────────────────── */

/*
 * guest_cap_token_t is an opaque seL4 badge returned by MSG_GUEST_BIND_DEVICE.
 * The device PD validates it on every subsequent IPC from the guest.
 * Tokens are revoked automatically by MSG_GUEST_DESTROY.
 */
typedef uint32_t guest_cap_token_t;

#define GUEST_CAP_TOKEN_INVALID  0u

/* ─── Guest state constants ──────────────────────────────────────────────── */

#define GUEST_STATE_CREATING   0u  /* slot allocated, not yet ready */
#define GUEST_STATE_BINDING    1u  /* device binding in progress */
#define GUEST_STATE_READY      2u  /* bound + memory set, awaiting BOOT */
#define GUEST_STATE_BOOTING    3u  /* BOOT sent, kernel not yet running */
#define GUEST_STATE_RUNNING    4u  /* guest OS executing */
#define GUEST_STATE_SUSPENDED  5u  /* suspended by MSG_GUEST_SUSPEND */
#define GUEST_STATE_DEAD       6u  /* terminated; no restart */

/* ─── Guest capabilities structure ──────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    guest_cap_token_t serial_token;  /* token from MSG_GUEST_BIND_DEVICE(SERIAL) */
    guest_cap_token_t net_token;     /* token from MSG_GUEST_BIND_DEVICE(NET) */
    guest_cap_token_t block_token;   /* token from MSG_GUEST_BIND_DEVICE(BLOCK) */
    guest_cap_token_t usb_token;     /* token from MSG_GUEST_BIND_DEVICE(USB) */
    guest_cap_token_t fb_token;      /* token from MSG_GUEST_BIND_DEVICE(FB) */
    uint32_t _reserved[3];
} guest_capabilities_t;

/* ─── Guest resource limits ──────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t cpu_budget_us;       /* MCS CPU budget per period */
    uint32_t cpu_period_us;       /* MCS scheduling period */
    uint32_t ram_mb;              /* guest RAM in megabytes */
    uint32_t net_bandwidth_kbps;  /* max network bandwidth (0 = unlimited) */
    uint32_t block_iops;          /* max block I/O operations per second (0 = unlimited) */
    uint32_t _reserved;
} guest_resource_limits_t;

/* ─── Device type constants ──────────────────────────────────────────────── */

#define GUEST_DEV_SERIAL   0u
#define GUEST_DEV_NET      1u
#define GUEST_DEV_BLOCK    2u
#define GUEST_DEV_USB      3u
#define GUEST_DEV_FB       4u
#define GUEST_DEV_COUNT    5u

#define GUEST_DEV_FLAG_SERIAL  (1u << GUEST_DEV_SERIAL)
#define GUEST_DEV_FLAG_NET     (1u << GUEST_DEV_NET)
#define GUEST_DEV_FLAG_BLOCK   (1u << GUEST_DEV_BLOCK)
#define GUEST_DEV_FLAG_USB     (1u << GUEST_DEV_USB)
#define GUEST_DEV_FLAG_FB      (1u << GUEST_DEV_FB)

/* ─── MSG_GUEST_CREATE ───────────────────────────────────────────────────── */

struct guest_create_req {
    uint32_t os_type;                /* VMM_OS_TYPE_* (see vmm_contract.h) */
    uint32_t device_flags;           /* GUEST_DEV_FLAG_* bitmask of intended devices */
    guest_resource_limits_t limits;
    uint8_t  label[16];              /* human-readable name for this guest slot */
};

struct guest_create_reply {
    uint32_t ok;
    uint32_t guest_id;               /* 0xFFFFFFFF = invalid / no slots */
};

/* ─── MSG_GUEST_BIND_DEVICE ──────────────────────────────────────────────── */

/*
 * Attach one device handle to the guest slot.  The ring-0 service PD validates
 * the caller's badge, allocates a cap_token, and records the binding.
 * Call once per device type before MSG_GUEST_BOOT.
 */

struct guest_bind_device_req {
    uint32_t guest_id;
    uint32_t dev_type;               /* GUEST_DEV_* constant */
    uint32_t dev_handle;             /* handle from MSG_{SERIAL,NET,BLOCK,USB,FB}_OPEN */
};

struct guest_bind_device_reply {
    uint32_t          ok;
    guest_cap_token_t cap_token;     /* token to pass to device PD on every IPC */
};

/* ─── MSG_GUEST_SET_MEMORY ───────────────────────────────────────────────── */

/*
 * Specify the guest's physical RAM range.  The root-task enforces the range
 * via seL4 MMU capabilities; the guest cannot access memory outside it.
 * Must be called exactly once per guest_id, after CREATE, before BOOT.
 */

struct guest_set_memory_req {
    uint32_t guest_id;
    uint32_t phys_base_lo;           /* low 32 bits of guest-physical base address */
    uint32_t phys_base_hi;           /* high 32 bits (for >4 GiB host physical space) */
    uint32_t size_mb;                /* RAM size in MiB; must match limits.ram_mb */
    uint32_t flags;                  /* GUEST_MEM_FLAG_* */
};

#define GUEST_MEM_FLAG_SHARED  (1u << 0)  /* map as shared (DMA-accessible) */
#define GUEST_MEM_FLAG_CACHED  (1u << 1)  /* normal cached mapping (default) */

struct guest_set_memory_reply {
    uint32_t ok;
    uint32_t actual_size_mb;         /* may differ if alignment padding required */
};

/* ─── MSG_GUEST_BOOT ─────────────────────────────────────────────────────── */

/*
 * Transfer control to the guest OS.  The VMM PD must have completed all
 * BIND_DEVICE and SET_MEMORY calls first; the ring-0 service validates state
 * before issuing the seL4 Resume on the guest vCPU.
 *
 * After BOOT succeeds the guest is GUEST_STATE_BOOTING; it transitions to
 * GUEST_STATE_RUNNING when the guest kernel signals readiness via EventBus.
 */

struct guest_boot_req {
    uint32_t guest_id;
    uint32_t entry_point_lo;         /* guest-physical entry point, low 32 bits */
    uint32_t entry_point_hi;         /* high 32 bits */
    uint32_t boot_arg;               /* passed to guest as first argument register */
};

struct guest_boot_reply {
    uint32_t ok;
};

/* ─── MSG_GUEST_SUSPEND ──────────────────────────────────────────────────── */

struct guest_suspend_req {
    uint32_t guest_id;
};

struct guest_suspend_reply {
    uint32_t ok;
    uint32_t state;                  /* GUEST_STATE_SUSPENDED on success */
};

/* ─── MSG_GUEST_RESUME ───────────────────────────────────────────────────── */

struct guest_resume_req {
    uint32_t guest_id;
};

struct guest_resume_reply {
    uint32_t ok;
    uint32_t state;                  /* GUEST_STATE_RUNNING on success */
};

/* ─── MSG_GUEST_DESTROY ──────────────────────────────────────────────────── */

/*
 * Destroy the guest slot and release all resources:
 *   - All cap_tokens for bound devices are revoked.
 *   - All device handles are closed.
 *   - The guest's MMU region is unmapped and memory returned to the pool.
 *   - The guest_id is invalidated and the slot made available for reuse.
 */

struct guest_destroy_req {
    uint32_t guest_id;
    uint32_t reason;                 /* GUEST_DESTROY_* reason code */
};

#define GUEST_DESTROY_NORMAL    0u   /* orderly shutdown */
#define GUEST_DESTROY_FORCED    1u   /* force-kill regardless of state */
#define GUEST_DESTROY_FAULT     2u   /* destroyed due to unhandled fault */

struct guest_destroy_reply {
    uint32_t ok;
};

/* ─── Internal binding protocol types (Layer B) ─────────────────────────── */

/*
 * Used by VMM PDs completing the self-registration binding sequence.
 */

struct guest_bind_req {
    uint32_t os_type;                /* VMM_OS_TYPE_* */
    uint32_t pd_id;                  /* seL4 badge / TRACE_PD_* of the VMM PD */
    guest_resource_limits_t limits;
    uint32_t device_flags;           /* GUEST_DEV_FLAG_* bitmask of requested devices */
};

struct guest_bind_reply {
    uint32_t ok;
    guest_capabilities_t caps;       /* populated tokens for bound devices */
};

/* ─── Guest lifecycle event (published to EventBus) ─────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t os_type;
    uint32_t guest_id;
    uint32_t pd_id;
    guest_capabilities_t caps;
} guest_ready_event_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum guest_error {
    GUEST_OK                      = 0,
    GUEST_ERR_BAD_OS_TYPE         = 1,
    GUEST_ERR_NO_SLOTS            = 2,   /* all guest slots occupied */
    GUEST_ERR_BAD_GUEST_ID        = 3,
    GUEST_ERR_BAD_DEV_TYPE        = 4,
    GUEST_ERR_DEVICE_UNAVAILABLE  = 5,   /* device PD has no free slots */
    GUEST_ERR_ALREADY_BOUND       = 6,   /* device already bound to this guest */
    GUEST_ERR_MEMORY_CONFLICT     = 7,   /* requested range overlaps another guest */
    GUEST_ERR_MEMORY_SIZE         = 8,   /* size_mb != limits.ram_mb */
    GUEST_ERR_NOT_READY           = 9,   /* BOOT before all required bindings done */
    GUEST_ERR_BAD_STATE           = 10,  /* operation invalid in current state */
    GUEST_ERR_QUOTA_REJECT        = 11,  /* QuotaPD rejected resource request */
    GUEST_ERR_PROTOCOL_VIOLATION  = 12,  /* step skipped in lifecycle protocol */
    GUEST_ERR_DEAD                = 13,  /* operation on DEAD guest */
};
