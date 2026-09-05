/*
 * vm_manager.c — Multi-VM lifecycle manager for agentOS
 * Copyright 2026, agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Passive PD (priority 145) that manages up to 4 simultaneous guest VMs.
 * Controller calls in via the vm_manager IPC endpoint.
 *
 * Opcodes (opcode in data[0..3]):
 *   OP_VM_CREATE    0x10  data[0]=VM_TYPE_* data[4]=ram_mb data[8]=flags
 *                         → ok, slot_id
 *   OP_VM_DESTROY   0x11  data[0]=slot_id → ok
 *   OP_VM_START     0x12  data[0]=slot_id → ok
 *   OP_VM_STOP      0x13  data[0]=slot_id → ok
 *   OP_VM_PAUSE     0x14  data[0]=slot_id → ok
 *   OP_VM_RESUME    0x15  data[0]=slot_id → ok
 *   OP_VM_CONSOLE   0x16  data[0]=slot_id → ok
 *   OP_VM_INFO      0x17  data[0]=slot_id
 *                         → ok, state, VM_TYPE_*, ram_mb, vcpu_count, uptime,
 *                           ram_vaddr, ram_gpa, device_flags
 *   OP_VM_LIST      0x18  → ok, count; vm_list_shmem has vm_list_entry_t[]
 *   OP_VM_SNAPSHOT  0x19  data[0]=slot_id → ok, snap_hash_lo, snap_hash_hi
 *   OP_VM_RESTORE   0x1A  data[0]=slot_id, [4]=snap_lo, [8]=snap_hi → ok
 *   OP_VM_CONFIGURE 0x1B  data[0]=slot_id data[4]=ram_mb
 *                         data[8]=cpu_budget_us data[12]=cpu_period_us → ok
 *   OP_VM_SEND_INPUT    0x1F data[0]=slot_id data[4..]=cc_input_event_t → ok
 *   OP_VM_CONSOLE_DRAIN 0x20 data[0]=slot_id data[4]=max
 *                         → ok, bytes, console bytes
 *   OP_VM_SET_QUOTA 0x30  data[0]=slot_id data[4]=cpu_pct → ok
 *   OP_VM_GET_STATS 0x31  data[0]=slot_id → ok, state, max_cpu_pct,
 *                         run_ticks_lo, run_ticks_hi,
 *                         preempt_count_lo, preempt_count_hi
 *   OP_VM_SET_AFFINITY 0x32 data[0]=slot_id data[4]=cpu_mask → ok
 *   OP_VM_INJECT_IRQ   0x33 data[0]=slot_id data[4]=irq_num  → ok
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/guest_contract.h"
#include "contracts/vibeos_contract.h"
#include "contracts/vm_manager_contract.h"
#include "system_desc.h"
/* vm_manager.h includes vmm_mux.h (found via -I../../freebsd-vmm in Makefile) */
#include "vm_manager.h"

/* ── Shared memory output region ─────────────────────────────────────────
 * vm_list_shmem (4KB) is mapped rw into this PD and r into controller.
 */
uintptr_t vm_list_vaddr;   /* set by linker (setvar_vaddr) */

/* ── Global VM multiplexer ─────────────────────────────────────────────── */
static vm_mux_t g_mux;
static uint8_t  g_vm_types[VM_MAX_SLOTS];
static uint32_t g_vm_flags[VM_MAX_SLOTS];
static seL4_CPtr g_linux_vmm_ep;
static seL4_CPtr g_freebsd_vmm_ep;
static seL4_CPtr g_slot_vmm_ep[VM_MAX_SLOTS];

/* ── Additional IPC opcodes (extend vmm_mux.h's OP_VM_* set) ──────────── */
#define OP_VM_SET_QUOTA    0x30u
#define OP_VM_GET_STATS    0x31u
#define OP_VM_SET_AFFINITY 0x32u
#define OP_VM_INJECT_IRQ   0x33u

/* ── Result codes ────────────────────────────────────────────────────────*/
#define VM_OK       0u
#define VM_ERR      1u
#define VM_NOT_IMPL 0xFEu

/* ── Per-slot CPU quota and scheduler state ──────────────────────────────*/
static vm_slot_quota_t g_quotas[VM_MAX_SLOTS];
static uint8_t g_sched_current = 0;
static uint32_t g_affinity[VM_MAX_SLOTS];

/* ── IPC-driven scheduler tick ───────────────────────────────────────────
 * Fire vm_sched_tick() every VM_SCHED_IPC_QUANTUM IPC dispatches.
 * Provides work-conserving credit scheduling without a hardware timer;
 * a real timer PD will replace this once timer-service is implemented.
 */
#define VM_SCHED_IPC_QUANTUM 16u
static uint32_t g_ipc_counter = 0;

/* ── msg helpers ────────────────────────────────────────────────────────*/
#ifndef AGENTOS_IPC_HELPERS_DEFINED
#define AGENTOS_IPC_HELPERS_DEFINED
static inline uint32_t msg_u32(const sel4_msg_t *m, uint32_t off) {
    uint32_t v = 0;
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        v  = (uint32_t)m->data[off]; v |= (uint32_t)m->data[off+1]<<8;
        v |= (uint32_t)m->data[off+2]<<16; v |= (uint32_t)m->data[off+3]<<24;
    }
    return v;
}
static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        m->data[off]=(uint8_t)v; m->data[off+1]=(uint8_t)(v>>8);
        m->data[off+2]=(uint8_t)(v>>16); m->data[off+3]=(uint8_t)(v>>24);
    }
}
#endif /* AGENTOS_IPC_HELPERS_DEFINED */
static inline void rep_u64(sel4_msg_t *m, uint32_t off, uint64_t v) {
    rep_u32(m, off,     (uint32_t)(v & 0xFFFFFFFFU));
    rep_u32(m, off + 4, (uint32_t)(v >> 32));
}

static inline void msg_put_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    rep_u32(m, off, v);
}

static inline uint32_t vm_arg_u32(const sel4_msg_t *req, uint32_t arg)
{
    uint32_t raw_off = arg * 4u;
    if (req->length >= raw_off + 4u)
        return msg_u32(req, raw_off);

    /* Compatibility with older tests/callers that encoded the opcode in
     * data[0] even though sel4_msg_t already carries req->opcode. */
    return msg_u32(req, raw_off + 4u);
}

static inline uint32_t vm_create_vm_type(const sel4_msg_t *req)
{
    uint32_t first = msg_u32(req, 0);
    if (first == VM_TYPE_LINUX || first == VM_TYPE_FREEBSD)
        return first;

    /* Legacy callers sent label_vaddr first.  There was no VM type field, so
     * preserve the old behavior by treating those requests as Linux. */
    return VM_TYPE_LINUX;
}

static inline uint32_t vm_create_ram_mb(const sel4_msg_t *req)
{
    uint32_t first = msg_u32(req, 0);
    if (first == VM_TYPE_LINUX || first == VM_TYPE_FREEBSD)
        return msg_u32(req, 4);

    /* Older tests encoded opcode,label,ram in data[0],data[4],data[8]. */
    if (first == OP_VM_CREATE)
        return msg_u32(req, 8);

    return msg_u32(req, 4);
}

static inline uint32_t vm_create_flags(const sel4_msg_t *req)
{
    uint32_t first = msg_u32(req, 0);
    if (first == VM_TYPE_LINUX || first == VM_TYPE_FREEBSD)
        return msg_u32(req, 8);
    return 0u;
}

static void vm_label_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    if (max == 0u) return;
    for (; i + 1u < max && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static seL4_CPtr vm_endpoint_for_type(uint32_t vm_type)
{
    return (vm_type == VM_TYPE_FREEBSD) ? g_freebsd_vmm_ep : g_linux_vmm_ep;
}

static uint32_t vm_service_for_type(uint32_t vm_type)
{
    return (vm_type == VM_TYPE_FREEBSD) ? SVC_ID_FREEBSD_VMM : SVC_ID_LINUX_VMM;
}

static uint32_t dedicated_vmm_os_type(uint32_t vm_type)
{
    return (vm_type == VM_TYPE_FREEBSD) ? 0x02u : 0x01u;
}

static uintptr_t dedicated_ram_base(uint32_t vm_type, uint8_t slot_id)
{
#if defined(AGENTOS_GUEST_BOTH)
    if (vm_type == VM_TYPE_FREEBSD)
        return 0x40000000UL;
    if (vm_type == VM_TYPE_LINUX)
        return 0xc0000000UL;
#else
    (void)vm_type;
#endif
    return VM_SLOT_RAM_BASE(slot_id);
}

static uint8_t dedicated_slot_for_type(uint32_t vm_type)
{
    if (g_linux_vmm_ep && g_freebsd_vmm_ep)
        return (vm_type == VM_TYPE_LINUX) ? 0u : 1u;
    return 0u;
}

static uint32_t dedicated_guest_rpc(uint8_t slot_id, uint32_t opcode,
                                    const uint8_t *payload,
                                    uint32_t payload_len,
                                    sel4_msg_t *out)
{
    if (slot_id >= VM_MAX_SLOTS || !g_slot_vmm_ep[slot_id])
        return VM_ERR;
    if (payload_len > SEL4_MSG_DATA_BYTES)
        return VM_ERR;

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = opcode;
    if (payload_len > 0u && payload != (const uint8_t *)0) {
        for (uint32_t i = 0; i < payload_len; i++)
            req.data[i] = payload[i];
    }
    req.length = payload_len;
    sel4_call(g_slot_vmm_ep[slot_id], &req, &rep);
    if (rep.opcode != GUEST_OK)
        return VM_ERR;
    if (out != (sel4_msg_t *)0)
        *out = rep;
    return VM_OK;
}

static uint32_t dedicated_guest_call(uint8_t slot_id, uint32_t opcode,
                                     uint32_t next_state)
{
    uint8_t payload[8u];
    uint32_t len = 4u;
    payload[0] = 0u; payload[1] = 0u; payload[2] = 0u; payload[3] = 0u;
    if (opcode == MSG_GUEST_DESTROY) {
        payload[4] = GUEST_DESTROY_NORMAL;
        payload[5] = 0u;
        payload[6] = 0u;
        payload[7] = 0u;
        len = 8u;
    }
    if (dedicated_guest_rpc(slot_id, opcode, payload, len, (sel4_msg_t *)0) != VM_OK)
        return VM_ERR;

    g_mux.slots[slot_id].state = (vm_slot_state_t)next_state;
    return VM_OK;
}

static int dedicated_create(uint32_t vm_type, uint32_t ram_mb,
                            uint32_t flags, uint8_t *slot_out)
{
    seL4_CPtr ep = vm_endpoint_for_type(vm_type);
    if (!ep)
        return -1;

    uint8_t slot_id = dedicated_slot_for_type(vm_type);
    if (slot_id >= VM_MAX_SLOTS)
        return -2;

    if (g_mux.slots[slot_id].state != VM_SLOT_FREE) {
        if (g_slot_vmm_ep[slot_id] == ep && g_vm_types[slot_id] == vm_type) {
            *slot_out = slot_id;
            return 0;
        }
        return -2;
    }

    /* Dedicated Linux/FreeBSD VMM PDs are single-guest services.  Their guest
     * image setup happens during VMM PD init, but the guest is not run until
     * vm_manager relays CREATE+BOOT.  That keeps dual-guest boots ordered and
     * makes the CC lifecycle call the real owner of guest startup.
     */
    vm_slot_t *slot = &g_mux.slots[slot_id];
    uintptr_t ram_base = dedicated_ram_base(vm_type, slot_id);
    slot->id = slot_id;
    slot->state = VM_SLOT_RUNNING;
    slot->ram_vaddr = ram_base;
    slot->ram_size = (ram_mb ? ((size_t)ram_mb << 20) : VM_SLOT_RAM_SIZE);
    slot->ram_paddr = ram_base;
    slot->vcpu_id = (uint32_t)slot_id;
    vm_label_copy(slot->label,
                  vm_type == VM_TYPE_FREEBSD ? "freebsd" : "linux",
                  (uint32_t)sizeof(slot->label));

    g_vm_types[slot_id] = (uint8_t)vm_type;
    g_vm_flags[slot_id] = flags;
    g_slot_vmm_ep[slot_id] = ep;
    g_mux.slot_count++;
    g_mux.active_slot = slot_id;

    {
        uint8_t payload[4u];
        uint32_t os_type = dedicated_vmm_os_type(vm_type);
        payload[0] = (uint8_t)(os_type & 0xffu);
        payload[1] = (uint8_t)((os_type >> 8u) & 0xffu);
        payload[2] = (uint8_t)((os_type >> 16u) & 0xffu);
        payload[3] = (uint8_t)((os_type >> 24u) & 0xffu);
        if (dedicated_guest_rpc(slot_id, MSG_GUEST_CREATE, payload,
                                (uint32_t)sizeof(payload),
                                (sel4_msg_t *)0) != VM_OK ||
            dedicated_guest_call(slot_id, MSG_GUEST_BOOT,
                                 VM_SLOT_RUNNING) != VM_OK) {
            g_slot_vmm_ep[slot_id] = 0u;
            g_vm_types[slot_id] = VM_TYPE_LINUX;
            g_vm_flags[slot_id] = 0u;
            slot->state = VM_SLOT_FREE;
            if (g_mux.slot_count > 0u)
                g_mux.slot_count--;
            return -3;
        }
    }

    *slot_out = slot_id;
    return 0;
}

static uint32_t dedicated_destroy(uint8_t slot_id)
{
    uint32_t rc = dedicated_guest_call(slot_id, MSG_GUEST_DESTROY, VM_SLOT_HALTED);
    if (rc != VM_OK)
        return VM_ERR;

    g_mux.slots[slot_id].state = VM_SLOT_FREE;
    g_slot_vmm_ep[slot_id] = 0u;
    g_vm_types[slot_id] = VM_TYPE_LINUX;
    g_vm_flags[slot_id] = 0u;
    if (g_mux.slot_count > 0u)
        g_mux.slot_count--;
    return VM_OK;
}

/* ── Helper: print a small decimal number without libc ──────────────────*/
static void dbg_u8(uint8_t v)
{
    char buf[4];
    buf[0] = '0' + (char)(v / 100 % 10);
    buf[1] = '0' + (char)(v / 10  % 10);
    buf[2] = '0' + (char)(v       % 10);
    buf[3] = '\0';
    if (buf[0] == '0' && buf[1] == '0') {
        sel4_dbg_puts(buf + 2);
    } else if (buf[0] == '0') {
        sel4_dbg_puts(buf + 1);
    } else {
        sel4_dbg_puts(buf);
    }
}

/* ── vm_sched_tick — round-robin scheduler ──────────────────────────────*/
void vm_sched_tick(vm_mux_t *mux)
{
    uint8_t cur = g_sched_current;

    for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
        vm_slot_state_t st = mux->slots[i].state;
        if (st == VM_SLOT_RUNNING || st == VM_SLOT_BOOTING) {
            g_quotas[i].run_ticks++;
        }
    }

    vm_slot_state_t cur_state = mux->slots[cur].state;
    bool cur_runnable = (cur_state == VM_SLOT_RUNNING ||
                         cur_state == VM_SLOT_BOOTING) &&
                        g_quotas[cur].max_cpu_pct > 0;

    if (!cur_runnable) {
        bool found = false;
        for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
            vm_slot_state_t s = mux->slots[i].state;
            if ((s == VM_SLOT_RUNNING || s == VM_SLOT_BOOTING) &&
                g_quotas[i].max_cpu_pct > 0) {
                g_sched_current = i;
                g_quotas[i].credits =
                    (int32_t)((uint32_t)g_quotas[i].max_cpu_pct *
                              SCHED_CREDITS_PER_PCT);
                found = true;
                break;
            }
        }
        (void)found;
        return;
    }

    g_quotas[cur].credits -= (int32_t)SCHED_CREDIT_QUANTUM;

    if (g_quotas[cur].credits > 0) return;

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
        g_quotas[cur].credits =
            (int32_t)((uint32_t)g_quotas[cur].max_cpu_pct *
                      SCHED_CREDITS_PER_PCT);
        return;
    }

    g_quotas[cur].preempt_count++;
    vmm_mux_pause(mux, cur);

    g_quotas[next].credits =
        (int32_t)((uint32_t)g_quotas[next].max_cpu_pct *
                  SCHED_CREDITS_PER_PCT);
    vmm_mux_resume(mux, next);

    g_sched_current = next;

    sel4_dbg_puts("[vm_manager] sched: preempted slot ");
    dbg_u8(cur);
    sel4_dbg_puts(" -> slot ");
    dbg_u8(next);
    sel4_dbg_puts("\n");
}

/* ── vm_set_quota ────────────────────────────────────────────────────────*/
int vm_set_quota(vm_mux_t *mux, uint8_t slot_id, uint8_t cpu_pct)
{
    (void)mux;
    if (slot_id >= VM_MAX_SLOTS) return -1;
    if (cpu_pct > 100u) cpu_pct = 100u;
    g_quotas[slot_id].max_cpu_pct = cpu_pct;
    g_quotas[slot_id].credits =
        (int32_t)((uint32_t)cpu_pct * SCHED_CREDITS_PER_PCT);
    return 0;
}

/* ── vm_get_stats ────────────────────────────────────────────────────────*/
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

    for (int i = 0; i < 15; i++) {
        out->label[i] = s->label[i];
        if (!s->label[i]) break;
    }
    out->label[15] = '\0';

    return 0;
}

/* ── vmm_set_affinity ────────────────────────────────────────────────────*/
int vmm_set_affinity(uint8_t slot_id, uint32_t cpu_mask)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;
    g_affinity[slot_id] = cpu_mask;
    sel4_dbg_puts("[vm_manager] affinity: slot ");
    dbg_u8(slot_id);
    sel4_dbg_puts(" cpu_mask set\n");
    /* TODO: call seL4_TCB_SetAffinity(vcpu_tcb_cap, cpu_mask) when available */
    return 0;
}

/* ── vmm_inject_irq ──────────────────────────────────────────────────────*/
int vmm_inject_irq(uint8_t slot_id, uint32_t irq_num)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;
    sel4_dbg_puts("[vm_manager] inject_irq: slot ");
    dbg_u8(slot_id);
    sel4_dbg_puts(" irq=");
    dbg_u8((uint8_t)(irq_num & 0xFF));
    sel4_dbg_puts(" (stub)\n");
    /* TODO: virq_inject(slot_vcpu_id, irq_num) via libvmm */
    return 0;
}

/* ── IPC handlers ────────────────────────────────────────────────────────*/

static uint32_t h_create(sel4_badge_t ba, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint32_t vm_type = vm_create_vm_type(req);
    uint32_t ram_mb  = vm_create_ram_mb(req);
    uint32_t flags   = vm_create_flags(req);
    const char *label = (vm_type == VM_TYPE_FREEBSD) ? "freebsd" : "linux";
    if (vm_type != VM_TYPE_LINUX && vm_type != VM_TYPE_FREEBSD) {
        rep_u32(rep, 0, VM_ERR);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }
    if (ram_mb != 0u && (ram_mb < 64u || (ram_mb & 3u) != 0u)) {
        rep_u32(rep, 0, VM_ERR);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    uint8_t slot_id = 0xFFu;
    int dedicated_rc = dedicated_create(vm_type, ram_mb, flags, &slot_id);
    if (dedicated_rc == -2) {
        rep_u32(rep, 0, VM_ERR);
        rep->length = 4;
        return SEL4_ERR_BUSY;
    }
    if (dedicated_rc == -3) {
        rep_u32(rep, 0, VM_ERR);
        rep->length = 4;
        return SEL4_ERR_INTERNAL;
    }
    if (dedicated_rc == 0) {
        sel4_dbg_puts("[vm_manager] CREATE dedicated slot=");
        dbg_u8(slot_id);
        sel4_dbg_puts("\n");
        rep_u32(rep, 0, VM_OK);
        rep_u32(rep, 4, (uint32_t)slot_id);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    slot_id = vmm_mux_create(&g_mux, label);
    if (slot_id == 0xFF) {
        sel4_dbg_puts("[vm_manager] CREATE failed: no free slots\n");
        rep_u32(rep, 0, VM_ERR); rep->length = 4;
        return SEL4_ERR_NO_MEM;
    }
    g_vm_types[slot_id] = (uint8_t)vm_type;
    g_vm_flags[slot_id] = flags;

    /* Rebalance quotas equally across all now-active slots.
     * Reset credits so existing slots don't carry excess credit from their
     * previous higher quota into the new smaller-quota regime. */
    {
        uint8_t active = g_mux.slot_count;
        uint8_t share  = (active > 0u) ? (uint8_t)(100u / active) : 100u;
        for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
            if (g_mux.slots[i].state != VM_SLOT_FREE) {
                g_quotas[i].max_cpu_pct = share;
                g_quotas[i].credits =
                    (int32_t)((uint32_t)share * SCHED_CREDITS_PER_PCT);
            }
        }
    }

    sel4_dbg_puts("[vm_manager] CREATE slot=");
    dbg_u8(slot_id);
    sel4_dbg_puts("\n");
    rep_u32(rep, 0, VM_OK);
    rep_u32(rep, 4, (uint32_t)slot_id);
    rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_destroy(sel4_badge_t ba, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t slot_id = (uint8_t)vm_arg_u32(req, 0);
    if (slot_id < VM_MAX_SLOTS && g_slot_vmm_ep[slot_id]) {
        uint32_t rc = dedicated_destroy(slot_id);
        rep_u32(rep, 0, rc);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
    int r = vmm_mux_destroy(&g_mux, slot_id);
    if (r == 0 && slot_id < VM_MAX_SLOTS) {
        g_vm_types[slot_id] = VM_TYPE_LINUX;
        g_vm_flags[slot_id] = 0u;
    }
    rep_u32(rep, 0, r == 0 ? VM_OK : VM_ERR);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_start(sel4_badge_t ba, const sel4_msg_t *req,
                          sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t slot_id = (uint8_t)vm_arg_u32(req, 0);
    uint32_t result = VM_ERR;
    if (slot_id < VM_MAX_SLOTS && g_mux.slots[slot_id].state != VM_SLOT_FREE) {
        if (g_slot_vmm_ep[slot_id]) {
            result = dedicated_guest_call(slot_id, MSG_GUEST_RESUME,
                                          VM_SLOT_RUNNING);
        } else if (g_mux.slots[slot_id].state == VM_SLOT_RUNNING ||
            g_mux.slots[slot_id].state == VM_SLOT_BOOTING) {
            result = VM_OK;
        } else {
            int r = vmm_mux_resume(&g_mux, slot_id);
            result = (r == 0) ? VM_OK : VM_ERR;
        }
    }
    rep_u32(rep, 0, result); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_stop(sel4_badge_t ba, const sel4_msg_t *req,
                         sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t slot_id = (uint8_t)vm_arg_u32(req, 0);
    uint32_t result = VM_ERR;
    if (slot_id < VM_MAX_SLOTS && g_mux.slots[slot_id].state != VM_SLOT_FREE) {
        if (g_slot_vmm_ep[slot_id]) {
            result = dedicated_guest_call(slot_id, MSG_GUEST_SUSPEND,
                                          VM_SLOT_SUSPENDED);
        } else {
            int r = vmm_mux_pause(&g_mux, slot_id);
            result = (r == 0) ? VM_OK : VM_ERR;
        }
    }
    rep_u32(rep, 0, result); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_pause(sel4_badge_t ba, const sel4_msg_t *req,
                          sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t slot_id = (uint8_t)vm_arg_u32(req, 0);
    if (slot_id < VM_MAX_SLOTS && g_slot_vmm_ep[slot_id]) {
        uint32_t rc = dedicated_guest_call(slot_id, MSG_GUEST_SUSPEND,
                                           VM_SLOT_SUSPENDED);
        rep_u32(rep, 0, rc);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
    int r = vmm_mux_pause(&g_mux, slot_id);
    rep_u32(rep, 0, r == 0 ? VM_OK : VM_ERR); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_resume(sel4_badge_t ba, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t slot_id = (uint8_t)vm_arg_u32(req, 0);
    if (slot_id < VM_MAX_SLOTS && g_slot_vmm_ep[slot_id]) {
        uint32_t rc = dedicated_guest_call(slot_id, MSG_GUEST_RESUME,
                                           VM_SLOT_RUNNING);
        rep_u32(rep, 0, rc);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
    int r = vmm_mux_resume(&g_mux, slot_id);
    rep_u32(rep, 0, r == 0 ? VM_OK : VM_ERR); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_console(sel4_badge_t ba, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t slot_id = (uint8_t)vm_arg_u32(req, 0);
    if (slot_id < VM_MAX_SLOTS && g_slot_vmm_ep[slot_id]) {
        g_mux.active_slot = slot_id;
        rep_u32(rep, 0, VM_OK);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
    int r = vmm_mux_switch(&g_mux, slot_id);
    rep_u32(rep, 0, r == 0 ? VM_OK : VM_ERR); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_send_input(sel4_badge_t ba, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t slot_id = (uint8_t)vm_arg_u32(req, 0);
    if (slot_id >= VM_MAX_SLOTS ||
        g_mux.slots[slot_id].state == VM_SLOT_FREE ||
        !g_slot_vmm_ep[slot_id]) {
        rep_u32(rep, 0, VM_ERR);
        rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }

    if (req->length < 28u || req->length > SEL4_MSG_DATA_BYTES - 4u) {
        rep_u32(rep, 0, VM_ERR);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }
    uint8_t payload[SEL4_MSG_DATA_BYTES];
    uint32_t input_len = req->length - 4u;
    payload[0] = 0u; payload[1] = 0u; payload[2] = 0u; payload[3] = 0u;
    for (uint32_t i = 0; i < input_len; i++) {
        payload[4u + i] = req->data[4u + i];
    }

    uint32_t rc = dedicated_guest_rpc(slot_id, MSG_GUEST_SEND_INPUT,
                                      payload, 4u + input_len,
                                      (sel4_msg_t *)0);
    rep_u32(rep, 0, rc);
    rep->length = 4;
    return rc == VM_OK ? SEL4_ERR_OK : SEL4_ERR_INTERNAL;
}

static uint32_t h_console_drain(sel4_badge_t ba, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t slot_id = (uint8_t)vm_arg_u32(req, 0);
    uint32_t max = vm_arg_u32(req, 1);
    if (max > VM_MANAGER_CONSOLE_INLINE_BYTES)
        max = VM_MANAGER_CONSOLE_INLINE_BYTES;

    if (slot_id >= VM_MAX_SLOTS ||
        g_mux.slots[slot_id].state == VM_SLOT_FREE ||
        !g_slot_vmm_ep[slot_id]) {
        rep_u32(rep, 0, VM_ERR);
        rep_u32(rep, 4, 0u);
        rep->length = 8;
        return SEL4_ERR_NOT_FOUND;
    }

    uint8_t payload[8u];
    payload[0] = 0u; payload[1] = 0u; payload[2] = 0u; payload[3] = 0u;
    payload[4] = (uint8_t)(max & 0xffu);
    payload[5] = (uint8_t)((max >> 8) & 0xffu);
    payload[6] = (uint8_t)((max >> 16) & 0xffu);
    payload[7] = (uint8_t)((max >> 24) & 0xffu);

    sel4_msg_t vrep = {0};
    uint32_t rc = dedicated_guest_rpc(slot_id, MSG_GUEST_CONSOLE_DRAIN,
                                      payload, (uint32_t)sizeof(payload),
                                      &vrep);
    if (rc != VM_OK) {
        rep_u32(rep, 0, VM_ERR);
        rep_u32(rep, 4, 0u);
        rep->length = 8;
        return SEL4_ERR_INTERNAL;
    }

    uint32_t n = vrep.length;
    if (n > max) n = max;
    if (n > VM_MANAGER_CONSOLE_INLINE_BYTES)
        n = VM_MANAGER_CONSOLE_INLINE_BYTES;

    rep_u32(rep, 0, VM_OK);
    rep_u32(rep, 4, n);
    for (uint32_t i = 0; i < n; i++)
        rep->data[8u + i] = vrep.data[i];
    rep->length = 8u + n;
    return SEL4_ERR_OK;
}

static uint32_t h_info(sel4_badge_t ba, const sel4_msg_t *req,
                         sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t slot_id = (uint8_t)vm_arg_u32(req, 0);
    if (slot_id >= VM_MAX_SLOTS) {
        rep_u32(rep, 0, VM_ERR); rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }
    vm_slot_t *s = &g_mux.slots[slot_id];
    rep_u32(rep, 0, VM_OK);
    rep_u32(rep, 4, (uint32_t)s->state);
    rep_u32(rep, 8, (uint32_t)g_vm_types[slot_id]);
    rep_u32(rep, 12, (uint32_t)(s->ram_size >> 20));
    rep_u32(rep, 16, 1u);
    rep_u64(rep, 20, 0u);
    rep_u64(rep, 28, (uint64_t)s->ram_vaddr);
    rep_u64(rep, 36, (uint64_t)s->ram_paddr);
    rep_u32(rep, 44, g_vm_flags[slot_id]);
    rep->length = 48;
    return SEL4_ERR_OK;
}

static uint32_t h_list(sel4_badge_t ba, const sel4_msg_t *req,
                         sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)req; (void)ctx;
    uint32_t count = 0;
    if (vm_list_vaddr) {
        vm_manager_list_entry_t *entries =
            (vm_manager_list_entry_t *)(uintptr_t)vm_list_vaddr;
        for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
            vm_slot_t *s = &g_mux.slots[i];
            if (s->state != VM_SLOT_FREE) {
                entries[count].slot_id = i;
                entries[count].state   = (uint8_t)s->state;
                entries[count].vm_type = g_vm_types[i];
                entries[count].reserved = 0;
                entries[count].ram_mb  = (uint32_t)(s->ram_size >> 20);
                for (int j = 0; j < 15; j++) {
                    entries[count].label[j] = s->label[j];
                    if (!s->label[j]) break;
                }
                entries[count].label[15] = '\0';
                entries[count].ram_gpa = (uint64_t)s->ram_paddr;
                entries[count].device_flags = g_vm_flags[i];
                entries[count].vmm_service_id = vm_service_for_type(g_vm_types[i]);
                count++;
            }
        }
    }
    rep_u32(rep, 0, VM_OK);
    rep_u32(rep, 4, count);
    rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_snapshot_restore(sel4_badge_t ba, const sel4_msg_t *req,
                                     sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)req; (void)ctx;
    sel4_dbg_puts("[vm_manager] SNAPSHOT/RESTORE: not implemented (Phase 1)\n");
    rep_u32(rep, 0, VM_NOT_IMPL); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_configure(sel4_badge_t ba, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t  slot_id       = (uint8_t)vm_arg_u32(req, 0);
    uint32_t new_ram_mb    = vm_arg_u32(req, 1);
    uint32_t cpu_budget_us = vm_arg_u32(req, 2);
    uint32_t cpu_period_us = vm_arg_u32(req, 3);

    if (slot_id >= VM_MAX_SLOTS ||
        g_mux.slots[slot_id].state == VM_SLOT_FREE) {
        rep_u32(rep, 0, VM_ERR);
        rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }

    uint32_t current_ram_mb =
        (uint32_t)(g_mux.slots[slot_id].ram_size >> 20);
    if (new_ram_mb != 0u && new_ram_mb != current_ram_mb) {
        rep_u32(rep, 0, VM_NOT_IMPL);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    if (cpu_budget_us != 0u && cpu_period_us != 0u) {
        uint32_t pct = (cpu_budget_us >= cpu_period_us)
                       ? 100u : (cpu_budget_us * 100u) / cpu_period_us;
        (void)vm_set_quota(&g_mux, slot_id, (uint8_t)pct);
    }

    rep_u32(rep, 0, VM_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_set_quota(sel4_badge_t ba, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t slot_id = (uint8_t)vm_arg_u32(req, 0);
    uint8_t cpu_pct = (uint8_t)vm_arg_u32(req, 1);
    int r = vm_set_quota(&g_mux, slot_id, cpu_pct);
    if (r == 0) {
        sel4_dbg_puts("[vm_manager] SET_QUOTA slot=");
        dbg_u8(slot_id);
        sel4_dbg_puts(" pct=");
        dbg_u8(cpu_pct);
        sel4_dbg_puts("\n");
    }
    rep_u32(rep, 0, r == 0 ? VM_OK : VM_ERR); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_get_stats(sel4_badge_t ba, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t slot_id = (uint8_t)vm_arg_u32(req, 0);
    vm_stats_t stats;
    int r = vm_get_stats(&g_mux, slot_id, &stats);
    if (r != 0) {
        rep_u32(rep, 0, VM_ERR); rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }
    rep_u32(rep, 0,  VM_OK);
    rep_u32(rep, 4,  (uint32_t)stats.state);
    rep_u32(rep, 8,  (uint32_t)stats.max_cpu_pct);
    rep_u64(rep, 12, stats.run_ticks);
    rep_u64(rep, 20, stats.preempt_count);
    rep->length = 28;
    return SEL4_ERR_OK;
}

static uint32_t h_set_affinity(sel4_badge_t ba, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t  slot_id  = (uint8_t)vm_arg_u32(req, 0);
    uint32_t cpu_mask = vm_arg_u32(req, 1);
    int r = vmm_set_affinity(slot_id, cpu_mask);
    rep_u32(rep, 0, r == 0 ? VM_OK : VM_ERR); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_inject_irq(sel4_badge_t ba, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)ba; (void)ctx;
    uint8_t  slot_id = (uint8_t)vm_arg_u32(req, 0);
    uint32_t irq_num = vm_arg_u32(req, 1);
    int r = vmm_inject_irq(slot_id, irq_num);
    rep_u32(rep, 0, r == 0 ? VM_OK : VM_ERR); rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── Nameserver registration ────────────────────────────────────────────
 * Make this PD discoverable by name so vibe_engine (and any future caller)
 * can resolve our endpoint via OP_NS_LOOKUP("vm_manager"), unblocking the
 * vibe_engine → vm_manager relay path.
 */
static void vm_manager_register_with_nameserver(seL4_CPtr ns_ep)
{
    if (!ns_ep) return;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xD0u;  /* OP_NS_REGISTER */
    /* data[0..3]   = channel_id (0 for dynamic PD) */
    /* data[4..7]   = pd_id (0 — assigned by nameserver) */
    /* data[8..11]  = cap_classes (0) */
    /* data[12..15] = version (1) */
    /* data[16..]   = name "vm_manager" */
    {
        uint8_t *d = req.data;
        d[ 0] = 0; d[ 1] = 0; d[ 2] = 0; d[ 3] = 0;
        d[ 4] = 0; d[ 5] = 0; d[ 6] = 0; d[ 7] = 0;
        d[ 8] = 0; d[ 9] = 0; d[10] = 0; d[11] = 0;
        d[12] = 1; d[13] = 0; d[14] = 0; d[15] = 0;
    }
    {
        const char nm[] = "vm_manager";
        for (int i = 0; nm[i] && (16 + i) < 48; i++)
            req.data[16 + i] = (uint8_t)nm[i];
    }
    req.length = 48;
    sel4_call(ns_ep, &req, &rep);
}

/* ── Entry point ────────────────────────────────────────────────────────*/

void vm_manager_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    vmm_mux_init(&g_mux);

#if defined(AGENTOS_GUEST_LINUX)
    g_linux_vmm_ep = (seL4_CPtr)PD_CNODE_SLOT_LINUX_VMM_EP;
#else
    g_linux_vmm_ep = 0u;
#endif
#if defined(AGENTOS_GUEST_FREEBSD)
    g_freebsd_vmm_ep = (seL4_CPtr)PD_CNODE_SLOT_FREEBSD_VMM_EP;
#else
    g_freebsd_vmm_ep = 0u;
#endif

    for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
        g_quotas[i].max_cpu_pct   = (uint8_t)(100u / VM_MAX_SLOTS);
        g_quotas[i].credits       = 0;
        g_quotas[i].run_ticks     = 0;
        g_quotas[i].preempt_count = 0;
        g_affinity[i]             = 0xFFFFFFFFu;
        g_vm_types[i]             = VM_TYPE_LINUX;
        g_vm_flags[i]             = 0u;
        g_slot_vmm_ep[i]          = 0u;
    }
    g_sched_current = 0;

    sel4_dbg_puts("[vm_manager] init: 4-slot VM multiplexer ready\n");
    sel4_dbg_puts("[vm_manager] scheduler: round-robin, 25% quota/slot\n");

    /* Publish under "vm_manager" so vibe_engine's lookup_service finds us. */
    vm_manager_register_with_nameserver(ns_ep);

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_VM_CREATE,       h_create,           (void *)0);
    sel4_server_register(&srv, OP_VM_DESTROY,      h_destroy,          (void *)0);
    sel4_server_register(&srv, OP_VM_START,        h_start,            (void *)0);
    sel4_server_register(&srv, OP_VM_STOP,         h_stop,             (void *)0);
    sel4_server_register(&srv, OP_VM_PAUSE,        h_pause,            (void *)0);
    sel4_server_register(&srv, OP_VM_RESUME,       h_resume,           (void *)0);
    sel4_server_register(&srv, OP_VM_CONSOLE,      h_console,          (void *)0);
    sel4_server_register(&srv, OP_VM_INFO,         h_info,             (void *)0);
    sel4_server_register(&srv, OP_VM_LIST,         h_list,             (void *)0);
    sel4_server_register(&srv, OP_VM_SNAPSHOT,     h_snapshot_restore, (void *)0);
    sel4_server_register(&srv, OP_VM_RESTORE,      h_snapshot_restore, (void *)0);
    sel4_server_register(&srv, OP_VM_CONFIGURE,    h_configure,        (void *)0);
    sel4_server_register(&srv, OP_VM_SEND_INPUT,   h_send_input,       (void *)0);
    sel4_server_register(&srv, OP_VM_CONSOLE_DRAIN,h_console_drain,    (void *)0);
    sel4_server_register(&srv, OP_VM_SET_QUOTA,    h_set_quota,        (void *)0);
    sel4_server_register(&srv, OP_VM_GET_STATS,    h_get_stats,        (void *)0);
    sel4_server_register(&srv, OP_VM_SET_AFFINITY, h_set_affinity,     (void *)0);
    sel4_server_register(&srv, OP_VM_INJECT_IRQ,   h_inject_irq,       (void *)0);
    /* Custom dispatch loop: fire vm_sched_tick() every VM_SCHED_IPC_QUANTUM
     * IPC dispatches.  Provides work-conserving credit scheduling without a
     * hardware timer; replace with a timer-notification path once timer-service
     * is implemented. */
    {
        sel4_msg_t   req   = {0};
        sel4_msg_t   rep   = {0};
        sel4_badge_t badge;
        int          first = 1;

        while (1) {
            if (first) {
                badge = sel4_recv(srv.ep, &req);
                first = 0;
            } else {
                badge = sel4_reply_recv(srv.ep, &rep, &req);
            }

            rep.opcode = 0;
            rep.length = 0;

            if (++g_ipc_counter >= VM_SCHED_IPC_QUANTUM) {
                g_ipc_counter = 0;
                vm_sched_tick(&g_mux);
            }

            sel4_server_dispatch(&srv, badge, &req, &rep);
        }
    }
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { vm_manager_main(my_ep, ns_ep); }
