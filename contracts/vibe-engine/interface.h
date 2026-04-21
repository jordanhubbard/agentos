/*
 * vibe-engine hot-swap interface — WASM service component lifecycle
 *
 * This header defines the IPC protocol between agents and the VibeEngine
 * protection domain (the "vibe-coding loop" server).
 *
 * The VibeEngine is conceptually separate from vibeOS (guest OS lifecycle):
 *   vibeOS    — create/destroy/configure full guest OS instances
 *   vibe-engine — hot-swap individual WASM service components within agentOS
 *
 * Hot-swap pipeline:
 *
 *   Agent generates or retrieves new WASM implementation
 *         │
 *         ▼  VSWAP_OP_PROPOSE
 *   Agent writes WASM bytes to staging region, calls PROPOSE
 *   VibeEngine records proposal, assigns proposal_id
 *         │
 *         ▼  VSWAP_OP_VALIDATE
 *   VibeEngine checks WASM magic, required exports (init, handle_ppc,
 *   health_check, notified), agentos.capabilities custom section, and
 *   capability declarations. Runs wasm_validator internally.
 *         │
 *         ▼  VSWAP_OP_COMMIT
 *   VibeEngine notifies the controller, which calls vibe_swap_begin():
 *     1. Copies WASM into a swap slot's shared-memory region
 *     2. Notifies the swap slot PD (which loads the module via wasm3)
 *     3. Runs conformance tests (health probe + service-specific ops)
 *     4. On success: redirects the service's IPC channel to the new slot
 *     5. Keeps old slot warm as rollback
 *         │
 *         ▼  VSWAP_OP_STATUS (query)
 *   Agent polls until VSWAP_STATE_ACTIVE or VSWAP_STATE_REJECTED
 *         │
 *         ▼  VSWAP_OP_ROLLBACK (optional, within rollback window)
 *   Controller redirects channel back to the warm rollback slot
 *
 * Endpoint discovery:
 *   The VibeEngine endpoint is available at nameserver path "vibe-engine.v1".
 *   Agents must hold a capability badge with the required VSWAP_RIGHT_* bits.
 *
 * Shared-memory staging region:
 *   The agent writes the WASM binary into the staging region before calling
 *   VSWAP_OP_PROPOSE. The staging region virtual address is communicated at
 *   capability-grant time (see VSWAP_SHMEM_SIZE). Only one pending proposal
 *   per agent is supported; calling PROPOSE again before COMMIT or ROLLBACK
 *   of the previous proposal returns VSWAP_ERR_BUSY.
 *
 * Kernel-side implementation:
 *   vibe_engine.c / vibe_swap.c in kernel/agentos-root-task/src/
 *   Userspace wasm_validator: userspace/servers/vibe-engine/src/wasm_validator.rs
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define VSWAP_API_VERSION_MAJOR  0
#define VSWAP_API_VERSION_MINOR  1
#define VSWAP_API_VERSION_PATCH  0

#define VSWAP_API_VERSION \
    ((VSWAP_API_VERSION_MAJOR << 16) | \
     (VSWAP_API_VERSION_MINOR <<  8) | \
      VSWAP_API_VERSION_PATCH)

/* ── Staging region ──────────────────────────────────────────────────────── */

/*
 * VSWAP_SHMEM_SIZE — size of the WASM staging shared-memory region.
 * Must match STAGING_SIZE in vibe_engine.c (0x400000 = 4 MiB).
 * The region is mapped into both the agent PD (write) and the VibeEngine
 * PD (read) by the Microkit system description.
 */
#define VSWAP_SHMEM_SIZE         UINT32_C(0x400000)   /* 4 MiB */
#define VSWAP_MAX_WASM_SIZE      (VSWAP_SHMEM_SIZE - 64u)

/* ── Service identifiers ─────────────────────────────────────────────────── */

/*
 * VSWAP_SVC_* — service IDs for swappable agentOS kernel services.
 *
 * These match the SVC_* constants in vibe_engine.c and vibe_swap.c.
 * Not all services are swappable — VSWAP_SVC_EVENTBUS is critical and
 * cannot be hot-swapped in v0.1.
 *
 * Use VSWAP_OP_LIST_SERVICES to discover which services support swapping
 * at runtime; do not hard-code swappability assumptions.
 */
#define VSWAP_SVC_EVENTBUS  UINT32_C(0)   /* NOT swappable in v0.1 */
#define VSWAP_SVC_MEMFS     UINT32_C(1)   /* swappable */
#define VSWAP_SVC_TOOLSVC   UINT32_C(2)   /* swappable */
#define VSWAP_SVC_MODELSVC  UINT32_C(3)   /* swappable */
#define VSWAP_SVC_AGENTFS   UINT32_C(4)   /* swappable */
#define VSWAP_SVC_LOGSVC    UINT32_C(5)   /* swappable */

#define VSWAP_SVC_MAX       UINT32_C(8)   /* upper bound (matches MAX_SERVICES) */

/* ── Proposal identifiers ────────────────────────────────────────────────── */

/*
 * vswap_proposal_id_t — opaque proposal identifier returned by VSWAP_OP_PROPOSE.
 * 0 is the invalid/null value (VSWAP_PROPOSAL_INVALID).
 * Valid IDs start at 1 and are monotonically increasing per VibeEngine boot.
 */
typedef uint32_t vswap_proposal_id_t;

#define VSWAP_PROPOSAL_INVALID  UINT32_C(0)

/* ── Proposal state ──────────────────────────────────────────────────────── */

/*
 * vswap_state_t — lifecycle state of a hot-swap proposal.
 *
 *   FREE        — slot available (internal; not returned to callers)
 *   PENDING     — submitted, awaiting validation
 *   VALIDATED   — WASM checks passed; ready for commit
 *   APPROVED    — commit issued; swap in progress
 *   ACTIVE      — swap complete; service is live at this version
 *   REJECTED    — validation or conformance testing failed
 *   ROLLEDBACK  — was active, then rolled back to a previous version
 *
 * State transitions:
 *   PENDING   → VALIDATED  (VSWAP_OP_VALIDATE succeeds)
 *   PENDING   → REJECTED   (VSWAP_OP_VALIDATE fails)
 *   VALIDATED → APPROVED   (VSWAP_OP_COMMIT issued)
 *   APPROVED  → ACTIVE     (swap slot passes conformance tests)
 *   APPROVED  → REJECTED   (conformance tests fail; auto-rollback)
 *   ACTIVE    → ROLLEDBACK (VSWAP_OP_ROLLBACK issued within rollback window)
 */
typedef enum __attribute__((packed)) {
    VSWAP_STATE_FREE        = 0,
    VSWAP_STATE_PENDING     = 1,
    VSWAP_STATE_VALIDATED   = 2,
    VSWAP_STATE_APPROVED    = 3,
    VSWAP_STATE_ACTIVE      = 4,
    VSWAP_STATE_REJECTED    = 5,
    VSWAP_STATE_ROLLEDBACK  = 6,
} vswap_state_t;

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

/*
 * Opcodes occupy the 0x5700 range to avoid collision with VOS_OP_* (0x5600)
 * and the kernel-side OP_VIBE_* (0x40-0x47).
 *
 * These are sent in MR0 of the seL4 PPC to the vibe-engine endpoint.
 */
#define VSWAP_OP_BASE             UINT32_C(0x5700)

#define VSWAP_OP_PROPOSE          (VSWAP_OP_BASE + 0x00u)
#define VSWAP_OP_VALIDATE         (VSWAP_OP_BASE + 0x01u)
#define VSWAP_OP_COMMIT           (VSWAP_OP_BASE + 0x02u)
#define VSWAP_OP_ROLLBACK         (VSWAP_OP_BASE + 0x03u)
#define VSWAP_OP_STATUS           (VSWAP_OP_BASE + 0x04u)
#define VSWAP_OP_LIST_SERVICES    (VSWAP_OP_BASE + 0x05u)
#define VSWAP_OP_HEALTH           (VSWAP_OP_BASE + 0x06u)

/* ── Error codes ─────────────────────────────────────────────────────────── */

typedef uint32_t vswap_err_t;

#define VSWAP_ERR_OK              UINT32_C(0)   /* Success                              */
#define VSWAP_ERR_FULL            UINT32_C(1)   /* Proposal table full (max 8 pending)  */
#define VSWAP_ERR_BADWASM         UINT32_C(2)   /* Invalid WASM header or magic         */
#define VSWAP_ERR_TOOBIG          UINT32_C(3)   /* Binary exceeds VSWAP_MAX_WASM_SIZE   */
#define VSWAP_ERR_NOSVC           UINT32_C(4)   /* service_id unknown or not swappable  */
#define VSWAP_ERR_NOENT           UINT32_C(5)   /* proposal_id not found                */
#define VSWAP_ERR_STATE           UINT32_C(6)   /* Proposal in wrong state for this op  */
#define VSWAP_ERR_VALFAIL         UINT32_C(7)   /* Validation failed (see val_flags)    */
#define VSWAP_ERR_BUSY            UINT32_C(8)   /* Another proposal pending for service */
#define VSWAP_ERR_NO_ROLLBACK     UINT32_C(9)   /* No rollback version available        */
#define VSWAP_ERR_PERMISSION      UINT32_C(10)  /* Caller badge lacks required right    */
#define VSWAP_ERR_INTERNAL        UINT32_C(99)  /* Unexpected internal error (bug)      */

/* ── Validation flag bits ────────────────────────────────────────────────── */

/*
 * vswap_val_flags_t — bitmask encoding which validation checks passed.
 *
 * Returned in the val_flags field of vswap_status_t and in MR2 by
 * VSWAP_OP_VALIDATE. A set bit means the check PASSED.
 *
 * Bit 0 — WASM magic bytes valid (\x00asm + version 1)
 * Bit 1 — binary size within VSWAP_MAX_WASM_SIZE
 * Bit 2 — target service exists and is swappable
 * Bit 3 — caller capability badge authorised for this service
 * Bit 4 — required exports present (init, handle_ppc, health_check, notified)
 * Bit 5 — WASM linear memory section declared
 * Bit 6 — agentos.capabilities custom section present
 * Bit 7 — all imports from known modules (aos.*, env)
 */
typedef uint32_t vswap_val_flags_t;

#define VSWAP_VAL_MAGIC      (1u << 0)
#define VSWAP_VAL_SIZE       (1u << 1)
#define VSWAP_VAL_SERVICE    (1u << 2)
#define VSWAP_VAL_CAP        (1u << 3)
#define VSWAP_VAL_EXPORTS    (1u << 4)
#define VSWAP_VAL_MEMORY     (1u << 5)
#define VSWAP_VAL_CAPS_SECT  (1u << 6)
#define VSWAP_VAL_IMPORTS    (1u << 7)

/* All required checks (bits 0-5); bits 6-7 are advisory only */
#define VSWAP_VAL_REQUIRED   (VSWAP_VAL_MAGIC   | \
                              VSWAP_VAL_SIZE    | \
                              VSWAP_VAL_SERVICE | \
                              VSWAP_VAL_CAP     | \
                              VSWAP_VAL_EXPORTS | \
                              VSWAP_VAL_MEMORY)

/* ── Structures ──────────────────────────────────────────────────────────── */

/*
 * vswap_status_t — proposal status reply for VSWAP_OP_STATUS.
 *
 * Written into the shared-memory region at offset 0 before the reply PPC
 * returns. Caller reads this after MR0 == VSWAP_ERR_OK.
 *
 * Fields:
 *   proposal_id   — echoes the queried proposal ID
 *   state         — current lifecycle state (see vswap_state_t)
 *   service_id    — target service (VSWAP_SVC_*)
 *   val_flags     — bitmask of validation checks that passed
 *   version       — monotonic version counter for the target service
 *                   (increments on each successful COMMIT for that service)
 *   wasm_size     — byte length of the proposed WASM binary
 *   _pad          — reserved, zero
 *   error_msg     — NUL-terminated human-readable rejection reason.
 *                   Valid only when state == VSWAP_STATE_REJECTED.
 *                   Empty string otherwise.
 */
typedef struct __attribute__((packed)) {
    vswap_proposal_id_t proposal_id;   /* 4 bytes */
    vswap_state_t       state;         /* 1 byte  */
    uint8_t             service_id;    /* 1 byte  */
    uint8_t             _pad[2];       /* 2 bytes */
    vswap_val_flags_t   val_flags;     /* 4 bytes */
    uint32_t            version;       /* 4 bytes */
    uint32_t            wasm_size;     /* 4 bytes */
    char                error_msg[48]; /* 48 bytes: rejection reason */
} vswap_status_t;                      /* total: 68 bytes */

/*
 * vswap_service_entry_t — one element in the array returned by
 * VSWAP_OP_LIST_SERVICES.
 *
 * Layout in shared memory (written by VSWAP_OP_LIST_SERVICES):
 *   [0x00] uint32_t count
 *   [0x04] vswap_service_entry_t entries[count]
 */
typedef struct __attribute__((packed)) {
    uint8_t   service_id;           /* 1 byte  */
    uint8_t   swappable;            /* 1 byte: non-zero if hot-swappable */
    uint8_t   _pad[2];              /* 2 bytes */
    uint32_t  current_version;      /* 4 bytes */
    uint32_t  max_wasm_bytes;       /* 4 bytes */
    char      name[16];             /* 16 bytes: NUL-terminated */
} vswap_service_entry_t;            /* total: 28 bytes */

/* ── Per-operation IPC layouts ───────────────────────────────────────────── */

/*
 * VSWAP_OP_PROPOSE
 *   Submit a new WASM binary for hot-swap consideration.
 *
 *   The agent must write the WASM binary to the staging region before this
 *   call. The VibeEngine reads from offset 0 in the staging region.
 *
 *   Input MRs:
 *     MR0 = VSWAP_OP_PROPOSE
 *     MR1 = service_id    (uint32_t, VSWAP_SVC_*)
 *     MR2 = wasm_size     (uint32_t, bytes in staging region)
 *     MR3 = cap_tag       (uint32_t, proposer's capability badge token)
 *
 *   Output MRs on success (MR0 == VSWAP_ERR_OK):
 *     MR1 = proposal_id   (vswap_proposal_id_t, non-zero)
 *
 *   Errors:
 *     VSWAP_ERR_FULL      — 8 proposals already pending; commit or reject one
 *     VSWAP_ERR_BADWASM   — staging region does not start with WASM magic
 *     VSWAP_ERR_TOOBIG    — wasm_size exceeds VSWAP_MAX_WASM_SIZE
 *     VSWAP_ERR_NOSVC     — service_id unknown
 *     VSWAP_ERR_BUSY      — another proposal is already pending for this service
 *     VSWAP_ERR_PERMISSION— caller badge lacks propose right
 *
 *   Note: PROPOSE only records the proposal. Validation does NOT run
 *   automatically; the agent must call VSWAP_OP_VALIDATE explicitly.
 *   This allows the agent to inspect/modify the binary before validation.
 */

/*
 * VSWAP_OP_VALIDATE
 *   Run all validation checks on a pending proposal.
 *
 *   Internally invokes the Rust wasm_validator (validate_wasm_report) to
 *   check WASM structural invariants, required exports, memory declarations,
 *   capability section presence, and import module hygiene.
 *
 *   A proposal that fails validation moves to VSWAP_STATE_REJECTED; the
 *   agent must call VSWAP_OP_PROPOSE again with a corrected binary.
 *
 *   Input MRs:
 *     MR0 = VSWAP_OP_VALIDATE
 *     MR1 = proposal_id   (vswap_proposal_id_t)
 *
 *   Output MRs on success (MR0 == VSWAP_ERR_OK, state → VALIDATED):
 *     MR1 = proposal_id   (echoed)
 *     MR2 = val_flags     (vswap_val_flags_t: all check bits set on clean pass)
 *
 *   Output MRs on validation failure (MR0 == VSWAP_ERR_VALFAIL):
 *     MR1 = proposal_id   (echoed)
 *     MR2 = val_flags     (bits clear for checks that failed)
 *     (error_msg in vswap_status_t shmem describes the first failure reason)
 *
 *   Errors:
 *     VSWAP_ERR_NOENT     — proposal_id not found
 *     VSWAP_ERR_STATE     — proposal not in PENDING state
 *     VSWAP_ERR_VALFAIL   — one or more required checks failed (see val_flags)
 */

/*
 * VSWAP_OP_COMMIT
 *   Approve a validated proposal and trigger the live hot-swap.
 *
 *   Transitions the proposal from VALIDATED → APPROVED, notifies the
 *   controller which executes vibe_swap_begin():
 *     1. Copies WASM into the swap slot shared-memory region
 *     2. Signals the swap slot PD to load the module via wasm3
 *     3. Runs conformance tests (health probe + storage R/W for SVC_MEMFS)
 *     4. On pass: atomically redirects the service IPC channel to the new slot
 *     5. Transitions proposal state → ACTIVE
 *     6. Keeps old slot warm as rollback (VSWAP_STATE_ROLLEDBACK on old proposal)
 *     On fail: slot reset to IDLE; proposal → REJECTED; old service untouched
 *
 *   Input MRs:
 *     MR0 = VSWAP_OP_COMMIT
 *     MR1 = proposal_id   (vswap_proposal_id_t, must be in VALIDATED state)
 *
 *   Output MRs on success (MR0 == VSWAP_ERR_OK):
 *     MR1 = proposal_id   (echoed)
 *     MR2 = new service version number (uint32_t)
 *
 *   Errors:
 *     VSWAP_ERR_NOENT     — proposal_id not found
 *     VSWAP_ERR_STATE     — proposal not in VALIDATED state
 *     VSWAP_ERR_VALFAIL   — conformance tests failed during swap; rolled back
 *     VSWAP_ERR_NOSVC     — service not found (should not happen post-validate)
 *     VSWAP_ERR_INTERNAL  — swap slot not available or controller unreachable
 *
 *   Note: COMMIT is asynchronous from the service-routing perspective — the
 *   channel redirect happens in the controller, not in the VibeEngine PD.
 *   Poll VSWAP_OP_STATUS until state is ACTIVE or REJECTED to confirm.
 */

/*
 * VSWAP_OP_ROLLBACK
 *   Revert a live service to its previous (warm rollback) version.
 *
 *   Only valid when a prior commit left a rollback slot warm. The rollback
 *   window is indefinite (the slot stays warm until overwritten by another
 *   successful commit or explicitly freed by a reclaim call).
 *
 *   Input MRs:
 *     MR0 = VSWAP_OP_ROLLBACK
 *     MR1 = service_id    (uint32_t, VSWAP_SVC_*) — identifies which service
 *                          to roll back, not a proposal_id.
 *
 *   Output MRs on success (MR0 == VSWAP_ERR_OK):
 *     MR1 = service_id    (echoed)
 *     MR2 = reverted-to version number (uint32_t)
 *
 *   Errors:
 *     VSWAP_ERR_NOSVC      — service_id unknown or not swappable
 *     VSWAP_ERR_NO_ROLLBACK— no warm rollback slot available for this service
 *     VSWAP_ERR_PERMISSION — caller badge lacks rollback right
 *
 *   Note: ROLLBACK for a proposal still in TESTING state (not yet ACTIVE)
 *   is handled by the controller which resets the slot to IDLE without a
 *   channel redirect. In that case MR2 reports the previous (unchanged) version.
 */

/*
 * VSWAP_OP_STATUS
 *   Query the current state of a specific proposal.
 *
 *   Input MRs:
 *     MR0 = VSWAP_OP_STATUS
 *     MR1 = proposal_id   (vswap_proposal_id_t)
 *
 *   Output (shared memory [0]) on success:
 *     vswap_status_t status
 *
 *   Output MRs on success:
 *     MR0 = VSWAP_ERR_OK
 *
 *   Errors:
 *     VSWAP_ERR_NOENT     — proposal_id not found
 */

/*
 * VSWAP_OP_LIST_SERVICES
 *   Enumerate all registered services and their swappability.
 *
 *   Input MRs:
 *     MR0 = VSWAP_OP_LIST_SERVICES
 *
 *   Output (shared memory [0]) on success:
 *     uint32_t count
 *     vswap_service_entry_t entries[count]
 *
 *   Output MRs on success:
 *     MR0 = VSWAP_ERR_OK
 *     MR1 = count (also in shmem for convenience)
 *
 *   count is bounded by VSWAP_SVC_MAX.
 */

/*
 * VSWAP_OP_HEALTH
 *   Health check for the VibeEngine itself (not a service under management).
 *   Returns engine-level counters useful for monitoring.
 *
 *   Input MRs:
 *     MR0 = VSWAP_OP_HEALTH
 *
 *   Output MRs on success (MR0 == VSWAP_ERR_OK):
 *     MR1 = total proposals received (uint32_t, wraps at UINT32_MAX)
 *     MR2 = total successful swaps   (uint32_t)
 *     MR3 = total rejections         (uint32_t)
 *     MR4 = active swap slots in use (uint32_t, 0..VSWAP_MAX_SLOTS)
 */

/* Maximum number of swap slots (matches MAX_SWAP_SLOTS in vibe_swap.c) */
#define VSWAP_MAX_SLOTS   4u

/* ── Capability rights ───────────────────────────────────────────────────── */

/*
 * vswap_rights_t — bitmask of rights encoded in a vibe-engine capability badge.
 *
 * VOS_VSWAP_RIGHT_PROPOSE   — may call VSWAP_OP_PROPOSE
 * VOS_VSWAP_RIGHT_VALIDATE  — may call VSWAP_OP_VALIDATE
 * VOS_VSWAP_RIGHT_COMMIT    — may call VSWAP_OP_COMMIT
 * VOS_VSWAP_RIGHT_ROLLBACK  — may call VSWAP_OP_ROLLBACK
 * VOS_VSWAP_RIGHT_STATUS    — may call VSWAP_OP_STATUS, VSWAP_OP_HEALTH
 * VOS_VSWAP_RIGHT_LIST      — may call VSWAP_OP_LIST_SERVICES
 */
typedef uint32_t vswap_rights_t;

#define VSWAP_RIGHT_PROPOSE   (1u << 0)
#define VSWAP_RIGHT_VALIDATE  (1u << 1)
#define VSWAP_RIGHT_COMMIT    (1u << 2)
#define VSWAP_RIGHT_ROLLBACK  (1u << 3)
#define VSWAP_RIGHT_STATUS    (1u << 4)
#define VSWAP_RIGHT_LIST      (1u << 5)

#define VSWAP_RIGHT_ALL       (VSWAP_RIGHT_PROPOSE  | \
                               VSWAP_RIGHT_VALIDATE | \
                               VSWAP_RIGHT_COMMIT   | \
                               VSWAP_RIGHT_ROLLBACK | \
                               VSWAP_RIGHT_STATUS   | \
                               VSWAP_RIGHT_LIST)

/* ── Static assertions (C11) ─────────────────────────────────────────────── */
#ifdef __STDC_VERSION__
#if __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(vswap_status_t)        == 68, "vswap_status_t size mismatch");
_Static_assert(sizeof(vswap_service_entry_t) == 28, "vswap_service_entry_t size mismatch");
_Static_assert(sizeof(vswap_state_t)         ==  1, "vswap_state_t must be 1 byte");
#endif
#endif
