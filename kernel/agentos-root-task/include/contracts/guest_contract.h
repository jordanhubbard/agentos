/* SPDX-License-Identifier: GPL-2.0-only */
/* contracts/guest_contract.h — Phase 3: Guest OS binding protocol
 *
 * Formal contract between agentOS and any guest OS running as a VMM PD.
 *
 * Binding protocol (in order):
 *   1. Guest announces to EventBus        (MSG_EVENTBUS_SUBSCRIBE)
 *   2. Guest queries device PDs           (MSG_AGENTFS_STAT on /devices)
 *   3. Guest opens device handles         (MSG_<DEVICE>_OPEN)
 *   4. Guest registers with QuotaPD       (MSG_QUOTA_REGISTER)
 *   5. Guest publishes ready event        (MSG_EVENTBUS_PUBLISH_BATCH, EVENT_GUEST_READY)
 *
 * Version history:
 *   1  —  initial definition (Phase 3)
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#define GUEST_CONTRACT_VERSION 1

/* -------------------------------------------------------------------------
 * Guest OS type identifiers
 * ---------------------------------------------------------------------- */
typedef enum {
    GUEST_TYPE_LINUX   = 0x01,
    GUEST_TYPE_FREEBSD = 0x02,
    GUEST_TYPE_NIXOS   = 0x03,
} guest_type_t;

/* -------------------------------------------------------------------------
 * Guest binding state — lifecycle of a VMM PD from init to ready
 * ---------------------------------------------------------------------- */
typedef enum {
    GUEST_STATE_INIT      = 0,  /* freshly created; no IPC calls made yet */
    GUEST_STATE_ANNOUNCED = 1,  /* subscribed to EventBus */
    GUEST_STATE_BOUND     = 2,  /* device handles acquired, QuotaPD registered */
    GUEST_STATE_READY     = 3,  /* EVENT_GUEST_READY published; accepting guest IPC */
    GUEST_STATE_DEAD      = 4,  /* terminated; all caps revoked */
} guest_state_t;

/* -------------------------------------------------------------------------
 * Device handle bitmask — which generic device PDs the guest has bound
 * ---------------------------------------------------------------------- */
#define GUEST_DEV_SERIAL  (1u << 0)
#define GUEST_DEV_NET     (1u << 1)
#define GUEST_DEV_BLOCK   (1u << 2)
#define GUEST_DEV_USB     (1u << 3)
#define GUEST_DEV_TIMER   (1u << 4)
#define GUEST_DEV_ENTROPY (1u << 5)

/* -------------------------------------------------------------------------
 * guest_capabilities_t — full set of device handles held by one guest OS.
 *
 * Filled by the VMM PD after completing steps 2–3 of the binding protocol.
 * Not transmitted directly over IPC; referenced by the VMM PD internally
 * and passed to QuotaPD only as a dev_mask summary.
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t dev_mask;       /* GUEST_DEV_* bitmask of successfully bound devices */
    uint32_t net_handle;     /* handle returned by net_pd OPEN, 0 if unbound */
    uint32_t block_handle;   /* handle returned by block_pd OPEN, 0 if unbound */
    uint32_t serial_handle;  /* handle returned by serial_pd OPEN, 0 if unbound */
    uint32_t timer_handle;   /* handle returned by timer_pd OPEN, 0 if unbound */
    uint32_t entropy_handle; /* handle returned by entropy_pd OPEN, 0 if unbound */
    uint8_t  _reserved[8];
} guest_capabilities_t;

/* -------------------------------------------------------------------------
 * guest_resource_limits_t — resource ceilings passed to QuotaPD at
 * MSG_QUOTA_REGISTER (binding step 4).
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t cpu_budget_us;  /* seL4 MCS budget per period, in microseconds */
    uint32_t cpu_period_us;  /* seL4 MCS scheduling period, in microseconds */
    uint32_t ram_mb;         /* maximum physical RAM allocation in MiB */
    uint32_t net_kbps;       /* network bandwidth ceiling in Kbps; 0 = unlimited */
    uint32_t disk_iops;      /* block I/O ceiling in IOPS; 0 = unlimited */
    uint8_t  _reserved[12];
} guest_resource_limits_t;

/* -------------------------------------------------------------------------
 * EventBus events published by guest VMM PDs
 * ---------------------------------------------------------------------- */
#define EVENT_GUEST_READY 0x4001u  /* all binding steps complete */
#define EVENT_GUEST_DEAD  0x4002u  /* guest terminated; handle is invalid */

/* -------------------------------------------------------------------------
 * Guest binding error codes
 * ---------------------------------------------------------------------- */
typedef enum {
    GUEST_OK                  = 0,
    GUEST_ERR_NO_SLOT         = 1,  /* no VMM slot available in root task */
    GUEST_ERR_DEV_UNAVAIL     = 2,  /* requested device PD is not ready */
    GUEST_ERR_QUOTA_EXCEEDED  = 3,  /* resource limits rejected by QuotaPD */
    GUEST_ERR_ALREADY_BOUND   = 4,  /* this guest_type is already registered */
    GUEST_ERR_BINDING_TIMEOUT = 5,  /* a binding step did not complete in time */
} guest_error_t;

/* -------------------------------------------------------------------------
 * Invariants (checked at runtime by the root task):
 *
 *  I1. A guest VMM PD may not invoke a device PD for which the corresponding
 *      GUEST_DEV_* bit in guest_capabilities_t.dev_mask is not set.
 *
 *  I2. All five binding steps must complete (and be acknowledged by the root
 *      task) before EVENT_GUEST_READY is published to the EventBus.
 *
 *  I3. guest_capabilities_t is local to the VMM PD's address space.  It is
 *      never transmitted raw over seL4 IPC; only the dev_mask summary is
 *      sent in MSG_QUOTA_REGISTER and MSG_EVENTBUS_PUBLISH_BATCH payloads.
 *
 *  I4. After EVENT_GUEST_DEAD, the root task reclaims all capabilities
 *      granted at slot allocation.  The VMM PD must not issue further IPC.
 * ---------------------------------------------------------------------- */
