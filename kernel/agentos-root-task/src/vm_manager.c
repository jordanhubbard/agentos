/*
 * vm_manager.c — Multi-VM lifecycle manager for agentOS
 * Copyright 2026, agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Passive PD (priority 145) that manages up to 4 simultaneous guest VMs.
 * Controller PPCs in via CH_VM_MANAGER (id=45).
 *
 * Opcodes (in MR0):
 *   OP_VM_CREATE    0x10  MR1=label_vaddr MR2=ram_mb → MR0=ok MR1=slot_id
 *   OP_VM_DESTROY   0x11  MR1=slot_id → MR0=ok
 *   OP_VM_START     0x12  MR1=slot_id → MR0=ok
 *   OP_VM_STOP      0x13  MR1=slot_id → MR0=ok
 *   OP_VM_PAUSE     0x14  MR1=slot_id → MR0=ok
 *   OP_VM_RESUME    0x15  MR1=slot_id → MR0=ok
 *   OP_VM_CONSOLE   0x16  MR1=slot_id → MR0=ok
 *   OP_VM_INFO      0x17  MR1=slot_id → MR0=ok MR1=state MR2=ram_vaddr
 *   OP_VM_LIST      0x18  → MR0=ok MR1=count; vm_list_shmem has vm_list_entry_t[]
 *   OP_VM_SNAPSHOT  0x19  MR1=slot_id → MR0=ok MR1=snap_hash_lo MR2=snap_hash_hi
 *   OP_VM_RESTORE   0x1A  MR1=slot_id MR2=snap_lo MR3=snap_hi → MR0=ok
 *   OP_VM_SET_QUOTA 0x1B  MR1=slot_id MR2=cpu_pct → MR0=ok
 *   OP_VM_GET_STATS 0x1C  MR1=slot_id → MR0=ok MR1=state MR2=max_cpu_pct
 *                         MR3=run_ticks_lo MR4=run_ticks_hi
 *                         MR5=preempt_count_lo MR6=preempt_count_hi
 *   OP_VM_SET_AFFINITY 0x1D MR1=slot_id MR2=cpu_mask → MR0=ok
 *   OP_VM_INJECT_IRQ   0x1E MR1=slot_id MR2=irq_num  → MR0=ok
 *
 * This PD wraps the vmm_mux multi-slot multiplexer (kernel/freebsd-vmm/vmm_mux.c)
 * and exposes a clean IPC API to the controller.
 *
 * Phase 1 note: vmm_mux depends on libvmm (au-ts/libvmm) which is only available
 * on AArch64 builds with GUEST_OS=freebsd.  On other platforms the IPC dispatch
 * layer still compiles and functions; vmm_mux_create() will fail gracefully
 * (returning 0xFF) if the underlying vCPU infrastructure is absent.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/vm_manager_contract.h"
/* vm_manager.h includes vmm_mux.h (found via -I../../freebsd-vmm in Makefile) */
#include "vm_manager.h"

/* ── Shared memory output region ─────────────────────────────────────────
 * vm_list_shmem (4KB) is mapped rw into this PD and r into controller.
 * Microkit sets vm_list_vaddr to the mapped virtual address at link time.
 */
uintptr_t vm_list_vaddr;   /* set by Microkit linker (setvar_vaddr) */

/* ── VM list entry written into vm_list_shmem on OP_VM_LIST ─────────────
 * Packed so the controller can read it without alignment surprises.
 */
typedef struct __attribute__((packed)) {
    uint8_t  slot_id;
    uint8_t  state;      /* VM_SLOT_* from vmm_mux.h */
    uint8_t  _pad[2];
    uint32_t ram_mb;
    char     label[16];
} vm_list_entry_t;

/* ── Global VM multiplexer ───────────────────────────────────────────────
 * One vm_mux_t manages all four slots.  Initialised in init().
 */
static vm_mux_t g_mux;

/* ── Additional IPC opcodes (extend vmm_mux.h's OP_VM_* set) ────────────── */
#define OP_VM_SET_QUOTA    0x1Bu  /* MR1=slot_id MR2=cpu_pct → MR0=ok        */
#define OP_VM_GET_STATS    0x1Cu  /* MR1=slot_id → MR0=ok + stats in MR1..6  */
#define OP_VM_SET_AFFINITY 0x1Du  /* MR1=slot_id MR2=cpu_mask → MR0=ok       */
#define OP_VM_INJECT_IRQ   0x1Eu  /* MR1=slot_id MR2=irq_num  → MR0=ok       */

/* ── Result codes ────────────────────────────────────────────────────────
 * Returned in MR0 for all operations.
 */
#define VM_OK       0u
#define VM_ERR      1u
#define VM_NOT_IMPL 0xFEu

/* ── Per-slot CPU quota and scheduler state ───────────────────────────────
 * Parallel array to g_mux.slots[].  Tracks credit counters, run statistics,
 * and the configured CPU share for each slot.  Default quota is 25% per slot
 * (100% / VM_MAX_SLOTS) so that all equal-priority guests share fairly.
 */
static vm_slot_quota_t g_quotas[VM_MAX_SLOTS];

/* Index of the slot that was running when the last sched tick fired.
 * Initialised to 0; updated by vm_sched_tick(). */
static uint8_t g_sched_current = 0;

/* Per-slot CPU affinity mask (bit N = allowed on core N).
 * 0xFFFFFFFF means "any core" (default). */
static uint32_t g_affinity[VM_MAX_SLOTS];

/* ── Helper: print a small decimal number without libc ──────────────────*/
static void dbg_u8(uint8_t v)
{
    char buf[4];
    buf[0] = '0' + (char)(v / 100 % 10);
    buf[1] = '0' + (char)(v / 10  % 10);
    buf[2] = '0' + (char)(v       % 10);
    buf[3] = '\0';
    /* Trim leading zeros for readability */
    if (buf[0] == '0' && buf[1] == '0') {
        microkit_dbg_puts(buf + 2);
    } else if (buf[0] == '0') {
        microkit_dbg_puts(buf + 1);
    } else {
        microkit_dbg_puts(buf);
    }
}

/* ── init — called once at PD startup ───────────────────────────────────*/

void init(void)
{
    vmm_mux_init(&g_mux);

    /* Initialise scheduler quota state */
    for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
        /* Equal default quota: 25% each (100 / VM_MAX_SLOTS) */
        g_quotas[i].max_cpu_pct  = (uint8_t)(100u / VM_MAX_SLOTS);
        g_quotas[i].credits      = 0;
        g_quotas[i].run_ticks    = 0;
        g_quotas[i].preempt_count = 0;
        g_affinity[i]            = 0xFFFFFFFFu; /* any core */
    }
    g_sched_current = 0;

    microkit_dbg_puts("[vm_manager] init: 4-slot VM multiplexer ready\n");
    microkit_dbg_puts("[vm_manager] scheduler: round-robin, 25% quota/slot\n");
}

/* ── vm_sched_tick — round-robin scheduler ──────────────────────────────
 *
 * Called from notified() on each timer tick.  Implements a simple credit-
 * based round-robin across RUNNING/BOOTING slots:
 *
 *   1. Deduct SCHED_CREDIT_QUANTUM from the current slot's credits.
 *   2. If credits > 0, keep the current slot running; return.
 *   3. If credits <= 0, find the next eligible slot (wrapping around).
 *   4. Suspend the exhausted slot, resume the new one, recharge credits.
 *
 * Slots with max_cpu_pct == 0 are ineligible (permanently paused by policy).
 * If no other eligible slot exists the current slot keeps running until
 * its credits are recharged on the next replenishment pass.
 */
void vm_sched_tick(vm_mux_t *mux)
{
    uint8_t cur = g_sched_current;

    /* Replenish credits for all slots at the start of each tick to keep
     * per-slot accounting monotonically increasing.  A slot whose credits
     * are already positive is not preempted this tick. */
    for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
        vm_slot_state_t st = mux->slots[i].state;
        if (st == VM_SLOT_RUNNING || st == VM_SLOT_BOOTING) {
            g_quotas[i].run_ticks++;
        }
    }

    /* Guard: if the current slot is no longer runnable, pick any runnable */
    vm_slot_state_t cur_state = mux->slots[cur].state;
    bool cur_runnable = (cur_state == VM_SLOT_RUNNING ||
                         cur_state == VM_SLOT_BOOTING) &&
                        g_quotas[cur].max_cpu_pct > 0;

    if (!cur_runnable) {
        /* Find any runnable slot to become current */
        bool found = false;
        for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
            vm_slot_state_t s = mux->slots[i].state;
            if ((s == VM_SLOT_RUNNING || s == VM_SLOT_BOOTING) &&
                g_quotas[i].max_cpu_pct > 0) {
                g_sched_current = i;
                /* Recharge credits for the newly selected slot */
                g_quotas[i].credits =
                    (int32_t)((uint32_t)g_quotas[i].max_cpu_pct *
                              SCHED_CREDITS_PER_PCT);
                found = true;
                break;
            }
        }
        (void)found; /* no runnable slot: idle */
        return;
    }

    /* Deduct one quantum from the current slot */
    g_quotas[cur].credits -= (int32_t)SCHED_CREDIT_QUANTUM;

    if (g_quotas[cur].credits > 0) {
        /* Slot still has budget — continue running */
        return;
    }

    /* Current slot exhausted: find next eligible slot in round-robin order */
    uint8_t next = cur;
    bool switched = false;
    for (uint8_t step = 1; step <= VM_MAX_SLOTS; step++) {
        uint8_t candidate = (uint8_t)((cur + step) % VM_MAX_SLOTS);
        vm_slot_state_t s = mux->slots[candidate].state;
        if ((s == VM_SLOT_RUNNING || s == VM_SLOT_BOOTING) &&
            g_quotas[candidate].max_cpu_pct > 0 &&
            candidate != cur) {
            next = candidate;
            switched = true;
            break;
        }
    }

    if (!switched) {
        /* Only one runnable slot: recharge and keep it running */
        g_quotas[cur].credits =
            (int32_t)((uint32_t)g_quotas[cur].max_cpu_pct *
                      SCHED_CREDITS_PER_PCT);
        return;
    }

    /* Suspend the exhausted slot, resume the next */
    g_quotas[cur].preempt_count++;
    vmm_mux_pause(mux, cur);

    /* Recharge credits for the incoming slot */
    g_quotas[next].credits =
        (int32_t)((uint32_t)g_quotas[next].max_cpu_pct *
                  SCHED_CREDITS_PER_PCT);
    vmm_mux_resume(mux, next);

    g_sched_current = next;

    microkit_dbg_puts("[vm_manager] sched: preempted slot ");
    dbg_u8(cur);
    microkit_dbg_puts(" -> slot ");
    dbg_u8(next);
    microkit_dbg_puts("\n");
}

/* ── vm_set_quota — configure per-slot CPU share ────────────────────────*/

int vm_set_quota(vm_mux_t *mux, uint8_t slot_id, uint8_t cpu_pct)
{
    (void)mux;
    if (slot_id >= VM_MAX_SLOTS) return -1;
    /* Clamp to 100 */
    if (cpu_pct > 100u) cpu_pct = 100u;
    g_quotas[slot_id].max_cpu_pct = cpu_pct;
    /* Reset credits so the new quota takes effect immediately */
    g_quotas[slot_id].credits =
        (int32_t)((uint32_t)cpu_pct * SCHED_CREDITS_PER_PCT);
    return 0;
}

/* ── vm_get_stats — fill stats for one slot ──────────────────────────────*/

int vm_get_stats(const vm_mux_t *mux, uint8_t slot_id, vm_stats_t *out)
{
    if (slot_id >= VM_MAX_SLOTS || !out) return -1;

    const vm_slot_t       *s = &mux->slots[slot_id];
    const vm_slot_quota_t *q = &g_quotas[slot_id];

    out->slot_id       = slot_id;
    out->state         = (uint8_t)s->state;
    out->max_cpu_pct   = q->max_cpu_pct;
    out->_pad          = 0;
    out->ram_mb        = (uint32_t)(s->ram_size >> 20);
    out->run_ticks     = q->run_ticks;
    out->preempt_count = q->preempt_count;

    /* Copy label */
    for (int i = 0; i < 15; i++) {
        out->label[i] = s->label[i];
        if (!s->label[i]) break;
    }
    out->label[15] = '\0';

    return 0;
}

/* ── vmm_set_affinity — store per-slot host CPU affinity mask ────────────
 *
 * The affinity mask is persisted in g_affinity[].  On platforms with
 * multi-core seL4 support it would be applied via seL4_TCB_SetAffinity
 * on the vCPU thread; this stub stores it for future enforcement.
 */
int vmm_set_affinity(uint8_t slot_id, uint32_t cpu_mask)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;
    g_affinity[slot_id] = cpu_mask;
    microkit_dbg_puts("[vm_manager] affinity: slot ");
    dbg_u8(slot_id);
    microkit_dbg_puts(" cpu_mask set\n");
    /*
     * TODO: call seL4_TCB_SetAffinity(vcpu_tcb_cap, cpu_mask) when
     * Microkit exposes TCB caps for virtual machine threads.
     */
    return 0;
}

/* ── vmm_inject_irq — stub for virtio IRQ injection ──────────────────────
 *
 * Logs the injection request.  A full implementation would call
 * virq_inject() from libvmm with the correct slot's vCPU context.
 */
int vmm_inject_irq(uint8_t slot_id, uint32_t irq_num)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;
    microkit_dbg_puts("[vm_manager] inject_irq: slot ");
    dbg_u8(slot_id);
    microkit_dbg_puts(" irq=");
    dbg_u8((uint8_t)(irq_num & 0xFF));
    microkit_dbg_puts(" (stub)\n");
    /*
     * TODO: resolve the vCPU for slot_id, then call:
     *   virq_inject(slot_vcpu_id, irq_num);
     * This requires libvmm being linked and the slot's vCPU initialised.
     */
    return 0;
}

/* ── protected — handles controller PPCs on CH_VM_MANAGER ───────────────*/

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg)
{
    (void)ch;
    (void)msg;

    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {

    /* ── OP_VM_CREATE ──────────────────────────────────────────────────── */
    case OP_VM_CREATE: {
        /*
         * MR1 = label vaddr (caller writes a NUL-terminated string into
         *        vm_list_shmem before the PPC, or passes NULL/0).
         * MR2 = ram_mb (informational for Phase 1 — actual RAM is
         *        fixed by the .system memory region assignments).
         */
        const char *label = (const char *)(uintptr_t)microkit_mr_get(1);
        /* Guard against NULL or obviously bad pointer */
        if (!label || (uintptr_t)label < 0x1000u) {
            label = "vm";
        }
        uint8_t slot_id = vmm_mux_create(&g_mux, label);
        if (slot_id == 0xFF) {
            microkit_dbg_puts("[vm_manager] CREATE failed: no free slots\n");
            microkit_mr_set(0, VM_ERR);
            return microkit_msginfo_new(0, 1);
        }

        /*
         * TODO (Phase 2 — OS-neutral capability grant for generic services):
         *
         * This is where vm_manager must grant the new VM's PD a capability to
         * each generic device service it is permitted to use, based on a
         * vm_spec/vm_flags bitmask that the controller passes in MR3 (to be
         * defined).  See docs/device-audit.md §5 for the full rationale.
         *
         * The enforcement rule: a VMM must use a generic service rather than
         * implementing its own device.  The binding happens here, once, at VM
         * creation, and is revoked when the slot is destroyed.
         *
         * Required channel additions to the .system file (Phase 2):
         *   CH_VM_MGR_TO_NET     — vm_manager -> net_server     (pp=true)
         *   CH_VM_MGR_TO_CONSOLE — vm_manager -> console_mux    (pp=true)
         *   CH_VM_MGR_TO_BLK     — vm_manager -> virtio_blk     (pp=true)
         *
         * Pseudocode for Phase 2 grant sequence (no code change yet):
         *
         *   uint32_t vm_flags = (uint32_t)microkit_mr_get(3);
         *
         *   if (vm_flags & VM_FLAG_NETWORK) {
         *       // PPC to net_server: OP_NET_VNIC_CREATE(slot_id, CAP_CLASS_NET)
         *       // On success: store assigned vnic_id in g_quotas[slot_id].
         *       // The vNIC is destroyed in the OP_VM_DESTROY handler.
         *   }
         *
         *   if (vm_flags & VM_FLAG_CONSOLE) {
         *       // PPC to console_mux: OP_CONSOLE_VM_REGISTER(slot_id)
         *       // console_mux allocates a session slot for this VM and returns
         *       // the ring index.  On OP_VM_CONSOLE, vm_manager also sends
         *       // OP_CONSOLE_ATTACH(slot_id) to console_mux so the session
         *       // multiplexer tracks which VM has active console focus.
         *   }
         *
         *   if (vm_flags & VM_FLAG_BLOCK) {
         *       // PPC to virtio_blk: OP_BLK_HEALTH to verify device is ready.
         *       // Store block channel ID in g_quotas[slot_id] so the VMM's
         *       // MMIO fault handler can route VirtIO block MMIO writes to the
         *       // correct PD via PPC rather than emulating locally.
         *   }
         *
         * Note: GPU shmem (gpu_tensor_buf MR) is approved as a custom channel
         * per docs/defects/DEFECT-001-gpu-shmem.md and does not go through this
         * generic grant path.  It is statically mapped in the .system file.
         */

        microkit_dbg_puts("[vm_manager] CREATE slot=");
        dbg_u8(slot_id);
        microkit_dbg_puts("\n");
        microkit_mr_set(0, VM_OK);
        microkit_mr_set(1, slot_id);
        return microkit_msginfo_new(0, 2);
    }

    /* ── OP_VM_DESTROY ─────────────────────────────────────────────────── */
    case OP_VM_DESTROY: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        int r = vmm_mux_destroy(&g_mux, slot_id);
        microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_START ───────────────────────────────────────────────────── */
    case OP_VM_START: {
        /*
         * Already running after CREATE; OP_VM_START is a resume-from-stopped
         * semantic (alias for vmm_mux_resume for Phase 1).
         */
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        if (slot_id >= VM_MAX_SLOTS ||
            g_mux.slots[slot_id].state == VM_SLOT_FREE) {
            microkit_mr_set(0, VM_ERR);
        } else if (g_mux.slots[slot_id].state == VM_SLOT_RUNNING ||
                   g_mux.slots[slot_id].state == VM_SLOT_BOOTING) {
            /* Already running — idempotent success */
            microkit_mr_set(0, VM_OK);
        } else {
            int r = vmm_mux_resume(&g_mux, slot_id);
            microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        }
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_STOP ────────────────────────────────────────────────────── */
    case OP_VM_STOP: {
        /*
         * Phase 1: treat stop as pause (not full destroy).
         * A stopped slot stays allocated but its vCPU is suspended.
         */
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        if (slot_id >= VM_MAX_SLOTS ||
            g_mux.slots[slot_id].state == VM_SLOT_FREE) {
            microkit_mr_set(0, VM_ERR);
        } else {
            int r = vmm_mux_pause(&g_mux, slot_id);
            microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        }
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_PAUSE ───────────────────────────────────────────────────── */
    case OP_VM_PAUSE: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        int r = vmm_mux_pause(&g_mux, slot_id);
        microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_RESUME ──────────────────────────────────────────────────── */
    case OP_VM_RESUME: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        int r = vmm_mux_resume(&g_mux, slot_id);
        microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_CONSOLE ─────────────────────────────────────────────────── */
    case OP_VM_CONSOLE: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        int r = vmm_mux_switch(&g_mux, slot_id);
        microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_INFO ────────────────────────────────────────────────────── */
    case OP_VM_INFO: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        if (slot_id >= VM_MAX_SLOTS) {
            microkit_mr_set(0, VM_ERR);
            return microkit_msginfo_new(0, 1);
        }
        vm_slot_t *s = &g_mux.slots[slot_id];
        microkit_mr_set(0, VM_OK);
        microkit_mr_set(1, (uint32_t)s->state);
        /* Only the low 32 bits of ram_vaddr fit in a single MR on 32-bit ABI */
        microkit_mr_set(2, (uint32_t)(s->ram_vaddr & 0xFFFFFFFFu));
        return microkit_msginfo_new(0, 3);
    }

    /* ── OP_VM_LIST ────────────────────────────────────────────────────── */
    case OP_VM_LIST: {
        uint32_t count = 0;
        if (vm_list_vaddr) {
            vm_list_entry_t *entries =
                (vm_list_entry_t *)(uintptr_t)vm_list_vaddr;
            for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
                vm_slot_t *s = &g_mux.slots[i];
                if (s->state != VM_SLOT_FREE) {
                    entries[count].slot_id = i;
                    entries[count].state   = (uint8_t)s->state;
                    entries[count]._pad[0] = 0;
                    entries[count]._pad[1] = 0;
                    entries[count].ram_mb  = (uint32_t)(s->ram_size >> 20);
                    /* Copy label (NUL-terminated, max 15 chars + NUL) */
                    for (int j = 0; j < 15; j++) {
                        entries[count].label[j] = s->label[j];
                        if (!s->label[j]) break;
                    }
                    entries[count].label[15] = '\0';
                    count++;
                }
            }
        }
        microkit_mr_set(0, VM_OK);
        microkit_mr_set(1, count);
        return microkit_msginfo_new(0, 2);
    }

    /* ── OP_VM_SNAPSHOT / OP_VM_RESTORE ───────────────────────────────── */
    case OP_VM_SNAPSHOT:
    case OP_VM_RESTORE:
        /*
         * Delegated to vm_snapshot PD (Phase 2).
         * Return NOT_IMPL so the controller can fall back gracefully.
         */
        microkit_dbg_puts("[vm_manager] SNAPSHOT/RESTORE: not implemented (Phase 1)\n");
        microkit_mr_set(0, VM_NOT_IMPL);
        return microkit_msginfo_new(0, 1);

    /* ── OP_VM_SET_QUOTA ──────────────────────────────────────────────── */
    case OP_VM_SET_QUOTA: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        uint8_t cpu_pct = (uint8_t)microkit_mr_get(2);
        int r = vm_set_quota(&g_mux, slot_id, cpu_pct);
        if (r == 0) {
            microkit_dbg_puts("[vm_manager] SET_QUOTA slot=");
            dbg_u8(slot_id);
            microkit_dbg_puts(" pct=");
            dbg_u8(cpu_pct);
            microkit_dbg_puts("\n");
        }
        microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_GET_STATS ──────────────────────────────────────────────── */
    case OP_VM_GET_STATS: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        vm_stats_t stats;
        int r = vm_get_stats(&g_mux, slot_id, &stats);
        if (r != 0) {
            microkit_mr_set(0, VM_ERR);
            return microkit_msginfo_new(0, 1);
        }
        microkit_mr_set(0, VM_OK);
        microkit_mr_set(1, (uint32_t)stats.state);
        microkit_mr_set(2, (uint32_t)stats.max_cpu_pct);
        /* run_ticks as two 32-bit halves */
        microkit_mr_set(3, (uint32_t)(stats.run_ticks & 0xFFFFFFFFu));
        microkit_mr_set(4, (uint32_t)(stats.run_ticks >> 32));
        /* preempt_count as two 32-bit halves */
        microkit_mr_set(5, (uint32_t)(stats.preempt_count & 0xFFFFFFFFu));
        microkit_mr_set(6, (uint32_t)(stats.preempt_count >> 32));
        return microkit_msginfo_new(0, 7);
    }

    /* ── OP_VM_SET_AFFINITY ───────────────────────────────────────────── */
    case OP_VM_SET_AFFINITY: {
        uint8_t  slot_id  = (uint8_t)microkit_mr_get(1);
        uint32_t cpu_mask = (uint32_t)microkit_mr_get(2);
        int r = vmm_set_affinity(slot_id, cpu_mask);
        microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_INJECT_IRQ ─────────────────────────────────────────────── */
    case OP_VM_INJECT_IRQ: {
        uint8_t  slot_id = (uint8_t)microkit_mr_get(1);
        uint32_t irq_num = (uint32_t)microkit_mr_get(2);
        int r = vmm_inject_irq(slot_id, irq_num);
        microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        return microkit_msginfo_new(0, 1);
    }

    default:
        microkit_dbg_puts("[vm_manager] unknown opcode\n");
        microkit_mr_set(0, VM_ERR);
        return microkit_msginfo_new(0, 1);
    }
}

/* ── notified — timer tick drives the scheduler ──────────────────────────
 *
 * Channel 0 (CH_SCHED_TICK): periodic timer notification from the system
 * timer PD.  Each notification is one scheduling quantum.  Any other channel
 * is logged and ignored.
 */
#define CH_SCHED_TICK  0u

void notified(microkit_channel ch)
{
    if (ch == CH_SCHED_TICK) {
        vm_sched_tick(&g_mux);
    } else {
        microkit_dbg_puts("[vm_manager] unexpected notification ch=");
        dbg_u8((uint8_t)ch);
        microkit_dbg_puts("\n");
    }
}
