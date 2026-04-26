/*
 * agentOS Quota Enforcement Protection Domain
 *
 * Passive PD (priority 115). Per-agent CPU/memory quota enforcement.
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/quota_pd_contract.h"

#define OP_QUOTA_REGISTER  0x60
#define OP_QUOTA_TICK      0x61
#define OP_QUOTA_STATUS    0x62
#define OP_QUOTA_SET       0x63

#define MAX_QUOTA_SLOTS  16

typedef struct {
    uint32_t agent_id, cpu_limit_ms, mem_limit_kb;
    uint32_t cpu_used_ms, mem_used_kb, flags;
    uint64_t tick_count, exceed_tick;
} quota_entry_t;

typedef struct __attribute__((packed)) {
    uint64_t seq, tick;
    uint32_t agent_id, event_type, value_a, value_b;
} quota_event_t;

typedef struct __attribute__((packed)) {
    uint32_t magic, version, slot_count, active_count;
    uint64_t event_head, event_count, event_capacity;
    uint8_t  _pad[24];
} quota_header_t;

#define QUOTA_MAGIC  0x0107A00D

uintptr_t quota_ring_vaddr;

#define QUOTA_HDR     ((volatile quota_header_t *)quota_ring_vaddr)
#define QUOTA_TABLE   ((volatile quota_entry_t *) \
    ((uint8_t *)quota_ring_vaddr + sizeof(quota_header_t)))
#define QUOTA_EVENTS  ((volatile quota_event_t *) \
    ((uint8_t *)quota_ring_vaddr + sizeof(quota_header_t) + \
     MAX_QUOTA_SLOTS * sizeof(quota_entry_t)))

static uint64_t boot_tick = 0;

static void put_dec(uint32_t v) {
    char buf[12]; int i = 11; buf[i] = '\0';
    if (v == 0) { log_drain_write(14, 14, "0"); return; }
    while (v > 0 && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    log_drain_write(14, 14, &buf[i]);
}

static void quota_pd_init(void) {
    volatile quota_header_t *hdr = QUOTA_HDR;
    uint64_t region_size = 0x40000;
    uint64_t table_size = sizeof(quota_header_t) + MAX_QUOTA_SLOTS * sizeof(quota_entry_t);
    uint64_t event_cap = (region_size - table_size) / sizeof(quota_event_t);
    hdr->magic = QUOTA_MAGIC; hdr->version = 1; hdr->slot_count = MAX_QUOTA_SLOTS;
    hdr->active_count = 0; hdr->event_head = 0; hdr->event_count = 0;
    hdr->event_capacity = event_cap;
    volatile quota_entry_t *table = QUOTA_TABLE;
    for (int i = 0; i < MAX_QUOTA_SLOTS; i++) {
        table[i].agent_id = 0; table[i].cpu_limit_ms = 0; table[i].mem_limit_kb = 0;
        table[i].cpu_used_ms = 0; table[i].mem_used_kb = 0; table[i].flags = 0;
        table[i].tick_count = 0; table[i].exceed_tick = 0;
    }
    log_drain_write(14, 14, "[quota_pd] Initialized: ");
    put_dec(MAX_QUOTA_SLOTS);
    log_drain_write(14, 14, " slots\n");
}

static void quota_log_event(uint32_t agent_id, uint32_t type, uint32_t a, uint32_t b) {
    volatile quota_header_t *hdr = QUOTA_HDR;
    volatile quota_event_t *events = QUOTA_EVENTS;
    uint64_t idx = hdr->event_head % hdr->event_capacity;
    volatile quota_event_t *e = &events[idx];
    e->seq = hdr->event_count; e->tick = boot_tick;
    e->agent_id = agent_id; e->event_type = type; e->value_a = a; e->value_b = b;
    hdr->event_head = (hdr->event_head + 1) % hdr->event_capacity;
    hdr->event_count++;
}

static int find_slot(uint32_t agent_id) {
    volatile quota_entry_t *t = QUOTA_TABLE;
    for (int i = 0; i < MAX_QUOTA_SLOTS; i++)
        if ((t[i].flags & QUOTA_FLAG_ACTIVE) && t[i].agent_id == agent_id) return i;
    return -1;
}
static int find_free_slot(void) {
    volatile quota_entry_t *t = QUOTA_TABLE;
    for (int i = 0; i < MAX_QUOTA_SLOTS; i++)
        if (!(t[i].flags & QUOTA_FLAG_ACTIVE)) return i;
    return -1;
}

static seL4_CPtr g_cap_broker_ep = 0;

static void revoke_agent_caps(uint32_t agent_id, uint32_t reason_flag) {
    if (g_cap_broker_ep != 0) {
#ifndef AGENTOS_TEST_HOST
        seL4_SetMR(0, MSG_QUOTA_REVOKE);
        seL4_SetMR(1, agent_id);
        seL4_SetMR(2, reason_flag);
        seL4_MessageInfo_t _i = seL4_MessageInfo_new(MSG_QUOTA_REVOKE, 0, 0, 3);
        (void)seL4_Call(g_cap_broker_ep, _i);
#endif
    }
    log_drain_write(14, 14, "[quota_pd] REVOKE agent=");
    put_dec(agent_id);
    log_drain_write(14, 14, "\n");
}

static void check_and_enforce(int slot_idx) {
    volatile quota_entry_t *entry = &QUOTA_TABLE[slot_idx];
    if (entry->flags & QUOTA_FLAG_REVOKED) return;
    bool exceeded = false;
    if (entry->cpu_limit_ms > 0 && entry->cpu_used_ms >= entry->cpu_limit_ms) {
        if (!(entry->flags & QUOTA_FLAG_CPU_EXCEED)) {
            entry->flags |= QUOTA_FLAG_CPU_EXCEED; exceeded = true;
            quota_log_event(entry->agent_id, 2, entry->cpu_used_ms, entry->cpu_limit_ms);
        }
    }
    if (entry->mem_limit_kb > 0 && entry->mem_used_kb >= entry->mem_limit_kb) {
        if (!(entry->flags & QUOTA_FLAG_MEM_EXCEED)) {
            entry->flags |= QUOTA_FLAG_MEM_EXCEED; exceeded = true;
            quota_log_event(entry->agent_id, 3, entry->mem_used_kb, entry->mem_limit_kb);
        }
    }
    if (exceeded) {
        entry->flags |= QUOTA_FLAG_REVOKED; entry->exceed_tick = boot_tick;
        uint32_t reason = 0;
        if (entry->flags & QUOTA_FLAG_CPU_EXCEED) reason |= QUOTA_FLAG_CPU_EXCEED;
        if (entry->flags & QUOTA_FLAG_MEM_EXCEED) reason |= QUOTA_FLAG_MEM_EXCEED;
        revoke_agent_caps(entry->agent_id, reason);
        quota_log_event(entry->agent_id, 4, reason, (uint32_t)boot_tick);
    }
}

/* msg helpers */
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

static uint32_t h_register(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t aid = msg_u32(req, 0), cpu = msg_u32(req, 4), mem = msg_u32(req, 8);
    int existing = find_slot(aid);
    if (existing >= 0) { rep_u32(rep, 0, (uint32_t)existing); rep_u32(rep, 4, 1); rep->length = 8; return SEL4_ERR_OK; }
    int slot = find_free_slot();
    if (slot < 0) { rep_u32(rep, 0, 0xFFFFFFFF); rep_u32(rep, 4, 0xE1); rep->length = 8; return SEL4_ERR_NO_MEM; }
    volatile quota_entry_t *entry = &QUOTA_TABLE[slot];
    entry->agent_id = aid; entry->cpu_limit_ms = cpu; entry->mem_limit_kb = mem;
    entry->cpu_used_ms = 0; entry->mem_used_kb = 0;
    entry->flags = QUOTA_FLAG_ACTIVE; entry->tick_count = 0; entry->exceed_tick = 0;
    QUOTA_HDR->active_count++;
    quota_log_event(aid, 1, cpu, mem);
    rep_u32(rep, 0, (uint32_t)slot); rep_u32(rep, 4, 0); rep->length = 8; return SEL4_ERR_OK;
}

static uint32_t h_tick(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t aid = msg_u32(req, 0), cpu_d = msg_u32(req, 4), mem_k = msg_u32(req, 8);
    int slot = find_slot(aid);
    if (slot < 0) { rep_u32(rep, 0, 0xE2); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    volatile quota_entry_t *entry = &QUOTA_TABLE[slot];
    entry->cpu_used_ms += cpu_d; entry->mem_used_kb = mem_k; entry->tick_count++;
    check_and_enforce(slot);
    rep_u32(rep, 0, 0); rep_u32(rep, 4, entry->flags); rep->length = 8; return SEL4_ERR_OK;
}

static uint32_t h_status(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    int slot = find_slot(msg_u32(req, 0));
    if (slot < 0) { rep_u32(rep, 0, 0xE2); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    volatile quota_entry_t *entry = &QUOTA_TABLE[slot];
    rep_u32(rep, 0,  entry->cpu_used_ms); rep_u32(rep, 4,  entry->cpu_limit_ms);
    rep_u32(rep, 8,  entry->mem_used_kb); rep_u32(rep, 12, entry->mem_limit_kb);
    rep_u32(rep, 16, entry->flags); rep->length = 20; return SEL4_ERR_OK;
}

static uint32_t h_set(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t aid = msg_u32(req, 0), ncpu = msg_u32(req, 4), nmem = msg_u32(req, 8);
    int slot = find_slot(aid);
    if (slot < 0) { rep_u32(rep, 0, 0xE2); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    volatile quota_entry_t *entry = &QUOTA_TABLE[slot];
    entry->cpu_limit_ms = ncpu; entry->mem_limit_kb = nmem;
    if (ncpu == 0 || entry->cpu_used_ms < ncpu) entry->flags &= ~QUOTA_FLAG_CPU_EXCEED;
    if (nmem == 0 || entry->mem_used_kb < nmem) entry->flags &= ~QUOTA_FLAG_MEM_EXCEED;
    if (!(entry->flags & (QUOTA_FLAG_CPU_EXCEED | QUOTA_FLAG_MEM_EXCEED)))
        entry->flags &= ~QUOTA_FLAG_REVOKED;
    quota_log_event(aid, 5, ncpu, nmem);
    rep_u32(rep, 0, 0); rep_u32(rep, 4, entry->flags); rep->length = 8; return SEL4_ERR_OK;
}

void quota_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    quota_pd_init();
    log_drain_write(14, 14, "[quota_pd] Ready — priority 115, passive\n");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_QUOTA_REGISTER, h_register, (void *)0);
    sel4_server_register(&srv, OP_QUOTA_TICK,     h_tick,     (void *)0);
    sel4_server_register(&srv, OP_QUOTA_STATUS,   h_status,   (void *)0);
    sel4_server_register(&srv, OP_QUOTA_SET,      h_set,      (void *)0);
    sel4_server_run(&srv);
}
