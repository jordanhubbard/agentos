/*
 * vm_snapshot.c — VM state snapshot and restore Protection Domain
 *
 * HURD-equivalent: checkpoint server (VM-level)
 * Priority: 130 (passive)
 *
 * Handles VM CPU state snapshot and restore via AgentFS.
 * Snapshot format: VMSN header + vCPU registers + guest RAM.
 *
 * Channel assignments (vm_snapshot's local view):
 *   id=0: receives PPC from controller (OP_VM_SNAPSHOT / OP_VM_RESTORE)
 *
 * Shared memory:
 *   vm_state_shmem (256MB): snapshot buffer; vm_snapshot writes on
 *   OP_VM_SNAPSHOT, controller reads for OP_VM_RESTORE staging.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/vm_snapshot_contract.h"

/* ── Output shared memory region (mapped by Microkit linker) ─────────── */
uintptr_t vm_state_vaddr;   /* 256MB max snapshot buffer */

/* ── Snapshot wire format ─────────────────────────────────────────────── */
#define SNAP_MAGIC   0x564D534Eu  /* "VMSN" */
#define SNAP_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint8_t  slot_id;
    uint8_t  _pad[3];
    uint64_t ram_size;
    uint64_t timestamp_ns;
    uint32_t vcpu_regs[32];   /* GP registers x0-x30 + SP */
    uint64_t pc;              /* program counter */
    /* RAM data follows inline */
} vm_snapshot_header_t;

_Static_assert(sizeof(vm_snapshot_header_t) == 164, "snapshot header size mismatch");

/* ── Monotonic tick counter ───────────────────────────────────────────── */
/*
 * Simple monotonic tick counter (incremented by controller timer notifies
 * if we ever add that channel; for now just counts operations).
 */
static uint64_t snap_tick = 0;

/* ── Per-slot snapshot metadata ───────────────────────────────────────── */
#define MAX_VM_SLOTS 4
static struct {
    bool     valid;
    uint8_t  slot_id;
    uint64_t timestamp_ns;
    uint64_t snap_hash_lo;
    uint64_t snap_hash_hi;
} snap_meta[MAX_VM_SLOTS];

/* ── FNV-1a 64-bit hash ───────────────────────────────────────────────── */
/*
 * Simple 64-bit hash over the snapshot buffer (FNV-1a 64-bit).
 * Returns two complementary 64-bit words as (lo, hi).
 */
static void snap_hash(const uint8_t *data, uint32_t len,
                      uint64_t *lo, uint64_t *hi)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    *lo = h;
    *hi = ~h;  /* complement as second word */
}

/* ═══════════════════════════════════════════════════════════════════════
 * Microkit entry points
 * ═══════════════════════════════════════════════════════════════════════ */

void init(void)
{
    /* Zero per-slot metadata */
    for (uint32_t i = 0; i < MAX_VM_SLOTS; i++) {
        snap_meta[i].valid        = false;
        snap_meta[i].slot_id      = 0;
        snap_meta[i].timestamp_ns = 0;
        snap_meta[i].snap_hash_lo = 0;
        snap_meta[i].snap_hash_hi = 0;
    }

    microkit_dbg_puts("[vm_snapshot] init: state region ");
    microkit_dbg_puts(vm_state_vaddr ? "mapped" : "NOT MAPPED");
    microkit_dbg_puts("\n");
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg)
{
    (void)ch;

    uint32_t op = (uint32_t)microkit_msginfo_get_label(msg);

    switch (op) {

    /* ── OP_VM_SNAPSHOT ──────────────────────────────────────────────── */
    case OP_VM_SNAPSHOT: {
        uint32_t slot_id = (uint32_t)microkit_mr_get(1);

        /* Validate slot_id */
        if (slot_id >= MAX_VM_SLOTS) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }

        /* Require mapped state region */
        if (!vm_state_vaddr) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }

        /* Build snapshot header at the start of the state region */
        volatile vm_snapshot_header_t *hdr =
            (volatile vm_snapshot_header_t *)vm_state_vaddr;

        hdr->magic   = SNAP_MAGIC;
        hdr->version = SNAP_VERSION;
        hdr->slot_id = (uint8_t)slot_id;
        hdr->_pad[0] = 0;
        hdr->_pad[1] = 0;
        hdr->_pad[2] = 0;

        /* Pseudo-timestamp: tick counter × 1ms expressed in ns */
        hdr->timestamp_ns = (++snap_tick) * 1000000ULL;

        /*
         * vCPU register state — simulation only.
         * A real implementation would call seL4_TCB_ReadRegisters on the
         * guest vCPU TCB cap to obtain the actual register file.
         */
        for (uint32_t r = 0; r < 32; r++) {
            hdr->vcpu_regs[r] = 0;
        }
        hdr->pc       = 0;

        /* 64MB RAM placeholder (real impl maps guest physical RAM) */
        hdr->ram_size = 64ULL * 1024ULL * 1024ULL;

        /*
         * Fill the inline RAM data area (immediately after the header) with
         * a fast pattern.  We only fill 64KB here to keep the snapshot call
         * bounded; a full implementation would DMA the entire guest RAM.
         */
        volatile uint8_t *ram_data =
            (volatile uint8_t *)(vm_state_vaddr + sizeof(vm_snapshot_header_t));
        for (uint32_t i = 0; i < 64u * 1024u; i++) {
            ram_data[i] = (uint8_t)((uint8_t)slot_id ^ (uint8_t)i);
        }

        /* Hash header + 64KB of RAM data */
        uint64_t hash_lo, hash_hi;
        snap_hash((const uint8_t *)vm_state_vaddr,
                  (uint32_t)sizeof(vm_snapshot_header_t) + 64u * 1024u,
                  &hash_lo, &hash_hi);

        /* Persist metadata for later restore verification */
        snap_meta[slot_id].valid        = true;
        snap_meta[slot_id].slot_id      = (uint8_t)slot_id;
        snap_meta[slot_id].timestamp_ns = hdr->timestamp_ns;
        snap_meta[slot_id].snap_hash_lo = hash_lo;
        snap_meta[slot_id].snap_hash_hi = hash_hi;

        microkit_dbg_puts("[vm_snapshot] snapshotted slot ");
        microkit_dbg_putc('0' + (char)slot_id);
        microkit_dbg_puts("\n");

        /* Return: MR0=ok, MR1=hash_lo (low 32 bits), MR2=hash_hi (low 32 bits) */
        microkit_mr_set(0, 0);
        microkit_mr_set(1, (uint32_t)(hash_lo & 0xFFFFFFFFu));
        microkit_mr_set(2, (uint32_t)(hash_hi & 0xFFFFFFFFu));
        return microkit_msginfo_new(0, 3);
    }

    /* ── OP_VM_RESTORE ───────────────────────────────────────────────── */
    case OP_VM_RESTORE: {
        uint32_t slot_id  = (uint32_t)microkit_mr_get(1);
        uint32_t snap_lo  = (uint32_t)microkit_mr_get(2);
        uint32_t snap_hi  = (uint32_t)microkit_mr_get(3);

        /* Validate slot_id and existence of a previous snapshot */
        if (slot_id >= MAX_VM_SLOTS || !snap_meta[slot_id].valid) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }

        /* Verify the caller's hash tokens match what we stored */
        uint32_t stored_lo = (uint32_t)(snap_meta[slot_id].snap_hash_lo & 0xFFFFFFFFu);
        uint32_t stored_hi = (uint32_t)(snap_meta[slot_id].snap_hash_hi & 0xFFFFFFFFu);

        if (snap_lo != stored_lo || snap_hi != stored_hi) {
            microkit_mr_set(0, 0xFEu);
            return microkit_msginfo_new(0, 1);
        }

        /*
         * "Restore": the snapshot buffer (vm_state_vaddr) still holds the
         * data written by the last OP_VM_SNAPSHOT.  In a real implementation
         * this would write the vCPU register file back via seL4_TCB_WriteRegisters
         * and DMA the RAM region back to the guest physical frames.
         */
        volatile vm_snapshot_header_t *hdr =
            (volatile vm_snapshot_header_t *)vm_state_vaddr;
        (void)hdr;  /* accessed in real impl; suppress unused warning */

        microkit_dbg_puts("[vm_snapshot] restored slot ");
        microkit_dbg_putc('0' + (char)slot_id);
        microkit_dbg_puts(" from snap ");
        /* Print snap_lo as a simple hex nibble hint */
        microkit_dbg_putc('0' + (char)((snap_lo >> 28) & 0xF));
        microkit_dbg_puts("\n");

        /* Return: MR0=ok, MR1=snap_lo, MR2=snap_hi (echo back for caller) */
        microkit_mr_set(0, 0);
        microkit_mr_set(1, snap_lo);
        microkit_mr_set(2, snap_hi);
        return microkit_msginfo_new(0, 3);
    }

    /* ── Unknown opcode ──────────────────────────────────────────────── */
    default:
        microkit_mr_set(0, 0xFFu);
        return microkit_msginfo_new(0, 1);
    }
}

void notified(microkit_channel ch)
{
    /* vm_snapshot receives no async notifications in the current design */
    (void)ch;
}
