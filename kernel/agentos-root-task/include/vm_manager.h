/*
 * vm_manager.h — Multi-VM lifecycle manager API for agentOS
 * Copyright 2026, agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Public interface for vm_manager.c:
 *   - Scheduler tick (round-robin across active slots)
 *   - Per-slot CPU quota management
 *   - Per-slot runtime statistics
 *   - Affinity and IRQ injection stubs (linux_vmm.c)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * vmm_mux.h is found via -I../../freebsd-vmm in the Makefile rule for
 * vm_manager.o.  It provides vm_mux_t, VM_MAX_SLOTS, vm_slot_state_t, etc.
 * Include it here so callers get the complete type.
 */
#include "vmm_mux.h"

/* ── Scheduler constants ─────────────────────────────────────────────────── */

/*
 * Credit quantum: each tick one SCHED_CREDIT_QUANTUM unit is deducted from
 * the running slot's credit counter.  A slot is preempted when credits reach
 * zero; credits are recharged from max_cpu_pct on the next scheduler round.
 */
#define SCHED_CREDIT_QUANTUM    1u
#define SCHED_CREDITS_PER_PCT   4u    /* credits per 1% of CPU quota per tick */

/* ── Per-slot CPU quota extension ───────────────────────────────────────── */
/*
 * vm_slot_quota_t is stored alongside each vm_slot_t in the scheduler's
 * parallel array g_quotas[VM_MAX_SLOTS].  Keeping it separate from vm_slot_t
 * avoids touching vmm_mux.h internals.
 */
typedef struct {
    uint8_t  max_cpu_pct;    /* 0-100: maximum CPU share for this slot */
    int32_t  credits;        /* current credit counter (replenished each round) */
    uint64_t run_ticks;      /* total scheduler ticks this slot was active */
    uint64_t preempt_count;  /* times this slot was preempted by the scheduler */
} vm_slot_quota_t;

/* ── Statistics returned by vm_get_stats() ───────────────────────────────── */
typedef struct {
    uint8_t  slot_id;
    uint8_t  state;          /* VM_SLOT_* from vmm_mux.h */
    uint8_t  max_cpu_pct;    /* configured quota */
    uint8_t  _pad;
    uint32_t ram_mb;         /* RAM size in MiB */
    uint64_t run_ticks;      /* total active scheduler ticks */
    uint64_t preempt_count;  /* preemption count */
    char     label[16];      /* human-readable label */
} vm_stats_t;

/* ── Scheduler API ───────────────────────────────────────────────────────── */

/**
 * vm_sched_tick — advance the round-robin scheduler by one tick.
 *
 * Called by the timer notification handler (or any periodic trigger) once per
 * scheduling quantum.  The scheduler deducts SCHED_CREDIT_QUANTUM from the
 * current running slot's credit counter.  When credits reach zero, it
 * suspends the current slot and resumes the next RUNNING slot in round-robin
 * order, replenishing its credits from max_cpu_pct.
 *
 * Slots with max_cpu_pct == 0 are skipped (they never receive CPU time from
 * the scheduler; they can still run if set RUNNING by external ops).
 *
 * @param mux   multiplexer state (g_mux in vm_manager.c)
 */
void vm_sched_tick(vm_mux_t *mux);

/**
 * vm_set_quota — configure a slot's CPU share.
 *
 * @param mux       multiplexer state
 * @param slot_id   target slot (0..VM_MAX_SLOTS-1)
 * @param cpu_pct   desired CPU share, 0-100
 * @returns 0 on success, -1 if slot_id is out of range
 */
int vm_set_quota(vm_mux_t *mux, uint8_t slot_id, uint8_t cpu_pct);

/**
 * vm_get_stats — fill a vm_stats_t for one slot.
 *
 * @param mux       multiplexer state
 * @param slot_id   target slot
 * @param out       caller-allocated vm_stats_t to fill
 * @returns 0 on success, -1 if slot_id is out of range
 */
int vm_get_stats(const vm_mux_t *mux, uint8_t slot_id, vm_stats_t *out);

/* ── linux_vmm affinity and IRQ injection API ────────────────────────────── */

/**
 * vmm_set_affinity — pin a guest VCPU to a specific host CPU.
 *
 * The affinity mask is stored in the per-slot quota state and applied by the
 * scheduler when the slot is next resumed.  On seL4 Microkit this translates
 * to a seL4_TCB_SetAffinity call on the vCPU thread; on platforms without
 * multi-core support the call is recorded but not enforced.
 *
 * @param slot_id   target slot (0..VM_MAX_SLOTS-1)
 * @param cpu_mask  bitmask of allowed host CPUs (bit N = core N)
 * @returns 0 on success, -1 if slot_id is out of range
 */
int vmm_set_affinity(uint8_t slot_id, uint32_t cpu_mask);

/**
 * vmm_inject_irq — inject a virtual IRQ into a guest slot.
 *
 * Stub for future virtio interrupt injection.  In a full implementation this
 * would call virq_inject() from libvmm on behalf of the target slot.  The
 * current implementation logs the request and returns 0 unconditionally so
 * callers can be written against the final API now.
 *
 * @param slot_id   target slot
 * @param irq_num   virtual IRQ number to inject (e.g. virtio queue IRQ)
 * @returns 0 (always succeeds in stub; will return -1 on hard error later)
 */
int vmm_inject_irq(uint8_t slot_id, uint32_t irq_num);
