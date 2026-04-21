/*
 * VM Snapshot IPC Contract
 *
 * vm_snapshot is a passive seL4 protection domain (priority 130) that handles
 * VM CPU-state snapshot and restore to/from AgentFS.  It checkpoints vCPU
 * register state and guest RAM, writing to a large shared buffer before
 * serialising to AgentFS.
 *
 * Channel: CH_VM_SNAPSHOT (46) — controller PPCs into vm_snapshot.
 *
 * Shared memory:
 *   vm_state_shmem (up to 256MB) — snapshot payload buffer.  On
 *   OP_VM_SNAPSHOT the server writes vCPU registers and guest RAM here
 *   before computing the content hash.  On OP_VM_RESTORE the controller
 *   places a previously retrieved snapshot here before calling.
 *
 * Invariants:
 *   - slot_id identifies a vm_manager slot (same namespace as OP_VM_CREATE).
 *   - hash_lo / hash_hi are the low and high 32-bit halves of a 64-bit
 *     content hash computed over vm_state_shmem after snapshot.
 *   - snap_lo / snap_hi in OP_VM_RESTORE identify the AgentFS inode or
 *     storage token of the snapshot to restore; the server retrieves the
 *     data into vm_state_shmem and then restores vCPU state.
 *   - SNAP_ERR_HASH is returned when the restored content hash does not
 *     match the stored hash (integrity check failure).
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define VM_SNAPSHOT_CH_CONTROLLER   CH_VM_SNAPSHOT   /* controller → vm_snapshot */

/* ─── Opcodes (placed in MR0) ────────────────────────────────────────────── */
/* Note: numeric values reuse OP_VM_SNAPSHOT/OP_VM_RESTORE from agentos.h */
#define OP_VM_SNAPSHOT_REQ   OP_VM_SNAPSHOT  /* 0x19u — checkpoint a running VM */
#define OP_VM_RESTORE_REQ    OP_VM_RESTORE   /* 0x1Au — restore a checkpointed VM */

/* ─── Request structs ────────────────────────────────────────────────────── */

/* OP_VM_SNAPSHOT
 * MR0=op, MR1=slot_id
 */
struct __attribute__((packed)) vm_snapshot_req_snapshot {
    uint32_t op;      /* OP_VM_SNAPSHOT */
    uint32_t slot_id; /* vm_manager slot to checkpoint */
};

/* OP_VM_RESTORE
 * MR0=op, MR1=slot_id, MR2=snap_lo, MR3=snap_hi
 */
struct __attribute__((packed)) vm_snapshot_req_restore {
    uint32_t op;      /* OP_VM_RESTORE */
    uint32_t slot_id; /* vm_manager slot to restore into */
    uint32_t snap_lo; /* low 32 bits of snapshot storage token */
    uint32_t snap_hi; /* high 32 bits of snapshot storage token */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

/* OP_VM_SNAPSHOT reply: MR0=ok, MR1=hash_lo, MR2=hash_hi */
struct __attribute__((packed)) vm_snapshot_reply_snapshot {
    uint32_t ok;       /* SNAP_OK or vm_snapshot_error */
    uint32_t hash_lo;  /* low 32 bits of content hash */
    uint32_t hash_hi;  /* high 32 bits of content hash */
};

/* OP_VM_RESTORE reply: MR0=ok, MR1=snap_lo, MR2=snap_hi */
struct __attribute__((packed)) vm_snapshot_reply_restore {
    uint32_t ok;       /* SNAP_OK or vm_snapshot_error */
    uint32_t snap_lo;  /* echoed storage token (low) */
    uint32_t snap_hi;  /* echoed storage token (high) */
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum vm_snapshot_error {
    SNAP_OK       = 0x00,
    SNAP_ERR      = 0xFF,   /* generic snapshot failure */
    SNAP_ERR_HASH = 0xFE,   /* integrity check failed on restore */
};
