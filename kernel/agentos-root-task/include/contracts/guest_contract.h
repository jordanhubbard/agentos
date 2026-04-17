/*
 * Guest OS IPC Contract
 *
 * Defines the formal interface between agentOS and any guest OS running as
 * a VMM Protection Domain.  This contract governs how guest OSes discover,
 * bind to, and use generic device PDs.
 *
 * All VMM PDs (linux_vmm, freebsd_vmm, future VMMs) must:
 *   1. Include this header.
 *   2. Complete the binding protocol below before running guest code.
 *   3. Expose a VirtIO transport backed by the generic device PDs.
 *   4. Not implement any device class for which a generic device PD exists.
 *
 * Binding Protocol (PLAN.md §3.1):
 *   1. Subscribe to EventBus for EVENT_GUEST_READY.
 *   2. Query AgentFS /devices namespace for available device PD endpoints.
 *   3. Send MSG_<DEVICE>_OPEN to each required device PD (badge-validated).
 *   4. Send MSG_QUOTA_REGISTER to QuotaPD with resource requirements.
 *   5. Publish MSG_EVENTBUS_PUBLISH_BATCH with EVENT_GUEST_READY.
 *
 * Invariant: A guest OS may not submit IPC to a device PD for which it does
 * not hold a valid handle (enforced by the device PD's client table).
 */

#pragma once
#include "../agentos.h"

/* ─── Guest capabilities structure ──────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t serial_handle;     /* handle from MSG_SERIAL_OPEN; 0 = not bound */
    uint32_t net_handle;        /* handle from MSG_NET_OPEN */
    uint32_t block_handle;      /* handle from MSG_BLOCK_OPEN */
    uint32_t usb_handle;        /* handle from MSG_USB_OPEN */
    uint32_t fb_handle;         /* handle from MSG_FB_OPEN */
    uint32_t _reserved[3];
} guest_capabilities_t;

/* ─── Guest resource limits ──────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t cpu_budget_us;     /* MCS CPU budget per period */
    uint32_t cpu_period_us;     /* MCS scheduling period */
    uint32_t ram_mb;            /* guest RAM in megabytes */
    uint32_t net_bandwidth_kbps; /* max network bandwidth (0 = unlimited) */
    uint32_t block_iops;        /* max block I/O operations per second (0 = unlimited) */
    uint32_t _reserved;
} guest_resource_limits_t;

/* ─── Guest binding request / reply ─────────────────────────────────────── */

struct guest_bind_req {
    uint32_t os_type;           /* VIBEOS_TYPE_* */
    uint32_t pd_id;             /* seL4 badge / TRACE_PD_* of the VMM PD */
    guest_resource_limits_t limits;
    uint32_t device_flags;      /* GUEST_DEV_FLAG_* bitmask of requested devices */
};

#define GUEST_DEV_FLAG_SERIAL  (1u << 0)
#define GUEST_DEV_FLAG_NET     (1u << 1)
#define GUEST_DEV_FLAG_BLOCK   (1u << 2)
#define GUEST_DEV_FLAG_USB     (1u << 3)
#define GUEST_DEV_FLAG_FB      (1u << 4)

struct guest_bind_reply {
    uint32_t ok;
    guest_capabilities_t caps;  /* populated handles for bound devices */
};

/* ─── Guest lifecycle event (published to EventBus) ─────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t os_type;
    uint32_t pd_id;
    guest_capabilities_t caps;
} guest_ready_event_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum guest_error {
    GUEST_OK                     = 0,
    GUEST_ERR_BAD_OS_TYPE        = 1,
    GUEST_ERR_DEVICE_UNAVAILABLE = 2,  /* requested device PD has no free slots */
    GUEST_ERR_QUOTA_REJECT       = 3,  /* QuotaPD rejected resource request */
    GUEST_ERR_ALREADY_BOUND      = 4,  /* binding protocol already completed */
    GUEST_ERR_PROTOCOL_VIOLATION = 5,  /* step skipped in binding protocol */
};
