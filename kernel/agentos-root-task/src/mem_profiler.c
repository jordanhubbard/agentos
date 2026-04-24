/*
 * agentOS MemProfiler Protection Domain
 *
 * Passive PD at priority 108. Tracks WASM heap allocations per agent slot.
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/mem_profiler_contract.h"
#include "prio_inherit.h"
#include <stdint.h>
#include <stdbool.h>

#define OP_MEM_REGISTER    0x60
#define OP_MEM_ALLOC_HOOK  0x61
#define OP_MEM_FREE_HOOK   0x62
#define OP_MEM_STATUS      0x63
#define OP_MEM_SNAPSHOT    0x64
#define OP_MEM_LEAK_ALERT  0x65

#define MEM_OK             0
#define MEM_ERR_BADSLOT    1
#define MEM_ERR_UNDERFLOW  2
#define MEM_ERR_INTERNAL   99

#define MAX_SLOTS              8
#define LEAK_THRESHOLD_TICKS  100
#define SNAPSHOT_INTERVAL      30

uintptr_t mem_ring_vaddr;

#define MEM_RING_SIZE          0x40000UL
#define MEM_RING_RECORD_SIZE   64
#define MEM_RING_CAPACITY      (MEM_RING_SIZE / MEM_RING_RECORD_SIZE)

typedef struct __attribute__((packed)) {
    uint64_t  tick;
    uint8_t   slot_id;
    uint8_t   _pad[3];
    uint32_t  live_allocs;
    uint64_t  live_bytes;
    uint64_t  total_allocs;
    uint64_t  total_frees;
    uint64_t  total_alloc_bytes;
    uint64_t  total_free_bytes;
    uint8_t   leak_suspected;
    uint8_t   _pad2[7];
} MemSnapshot;

_Static_assert(sizeof(MemSnapshot) == MEM_RING_RECORD_SIZE,
               "MemSnapshot must be exactly 64 bytes");

typedef struct {
    bool     registered;
    uint32_t live_allocs;
    uint64_t live_bytes;
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t total_alloc_bytes;
    uint64_t total_free_bytes;
    uint32_t prev_live_allocs;
    uint32_t monotonic_ticks;
    bool     leak_alerted;
} SlotState;

static struct {
    SlotState slots[MAX_SLOTS];
    uint64_t  tick;
    uint32_t  ring_head;
    bool      ring_ready;
} mpstate;

static seL4_CPtr g_ctrl_ep = 0;

static void put_str(const char *s) { log_drain_write(8, 8, s); }
static void put_dec(uint64_t v) {
    if (v == 0) { put_str("0"); return; }
    char buf[21]; int i = 20; buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (int)(v % 10); v /= 10; }
    put_str(&buf[i]);
}
static bool valid_slot(uint32_t slot) { return slot < MAX_SLOTS; }

static void ring_write(uint8_t slot_id) {
    if (!mpstate.ring_ready) return;
    SlotState *s = &mpstate.slots[slot_id];
    MemSnapshot *ring = (MemSnapshot *)(uintptr_t)mem_ring_vaddr;
    uint32_t idx = mpstate.ring_head % MEM_RING_CAPACITY;
    MemSnapshot *rec = &ring[idx];
    rec->tick              = mpstate.tick;
    rec->slot_id           = slot_id;
    rec->live_allocs       = s->live_allocs;
    rec->live_bytes        = s->live_bytes;
    rec->total_allocs      = s->total_allocs;
    rec->total_frees       = s->total_frees;
    rec->total_alloc_bytes = s->total_alloc_bytes;
    rec->total_free_bytes  = s->total_free_bytes;
    rec->leak_suspected    = s->monotonic_ticks >= LEAK_THRESHOLD_TICKS ? 1 : 0;
    rec->_pad[0] = rec->_pad[1] = rec->_pad[2] = 0;
    for (int j = 0; j < 8; j++) rec->_pad2[j] = 0;
    mpstate.ring_head++;
}

static void snapshot_all(void) {
    for (uint8_t i = 0; i < MAX_SLOTS; i++)
        if (mpstate.slots[i].registered) ring_write(i);
    put_str("[mem_profiler] snapshot tick=");
    put_dec(mpstate.tick);
    put_str("\n");
}

static void check_leak(uint8_t slot_id) {
    SlotState *s = &mpstate.slots[slot_id];
    if (!s->registered || s->leak_alerted) return;
    if (s->live_allocs > s->prev_live_allocs) s->monotonic_ticks++;
    else s->monotonic_ticks = 0;
    s->prev_live_allocs = s->live_allocs;
    if (s->monotonic_ticks >= LEAK_THRESHOLD_TICKS) {
        put_str("[mem_profiler] LEAK DETECTED slot="); put_dec(slot_id); put_str("\n");
        if (g_ctrl_ep != 0) {
#ifndef AGENTOS_TEST_HOST
            seL4_SetMR(0, OP_MEM_LEAK_ALERT);
            seL4_SetMR(1, slot_id);
            seL4_SetMR(2, s->live_allocs);
            seL4_SetMR(3, (uint32_t)(s->live_bytes >> 32));
            seL4_SetMR(4, (uint32_t)(s->live_bytes & 0xFFFFFFFF));
            seL4_MessageInfo_t _i = seL4_MessageInfo_new(OP_MEM_LEAK_ALERT, 0, 0, 5);
            (void)seL4_Call(g_ctrl_ep, _i);
#endif
        }
        s->leak_alerted = true;
    }
}

/* msg helpers */
static inline uint32_t msg_u32(const sel4_msg_t *m, uint32_t off) {
    uint32_t v = 0;
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        v  = (uint32_t)m->data[off];
        v |= (uint32_t)m->data[off+1] << 8;
        v |= (uint32_t)m->data[off+2] << 16;
        v |= (uint32_t)m->data[off+3] << 24;
    }
    return v;
}
static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        m->data[off]   = (uint8_t)v;
        m->data[off+1] = (uint8_t)(v >> 8);
        m->data[off+2] = (uint8_t)(v >> 16);
        m->data[off+3] = (uint8_t)(v >> 24);
    }
}

static uint32_t handle_register(sel4_badge_t b, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t slot = msg_u32(req, 0);
    if (!valid_slot(slot)) { rep_u32(rep, 0, MEM_ERR_BADSLOT); rep->length = 4; return MEM_ERR_BADSLOT; }
    SlotState *s = &mpstate.slots[slot];
    s->registered = true; s->live_allocs = 0; s->live_bytes = 0;
    s->total_allocs = 0; s->total_frees = 0;
    s->total_alloc_bytes = 0; s->total_free_bytes = 0;
    s->prev_live_allocs = 0; s->monotonic_ticks = 0; s->leak_alerted = false;
    put_str("[mem_profiler] slot="); put_dec(slot); put_str(" registered\n");
    rep_u32(rep, 0, MEM_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t handle_alloc(sel4_badge_t b, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t slot = msg_u32(req, 0);
    uint32_t size = msg_u32(req, 4);
    if (!valid_slot(slot)) { rep_u32(rep, 0, MEM_ERR_BADSLOT); rep->length = 4; return MEM_ERR_BADSLOT; }
    SlotState *s = &mpstate.slots[slot];
    s->live_allocs++; s->live_bytes += size;
    s->total_allocs++; s->total_alloc_bytes += size;
    rep_u32(rep, 0, MEM_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t handle_free(sel4_badge_t b, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t slot = msg_u32(req, 0);
    uint32_t size = msg_u32(req, 4);
    if (!valid_slot(slot)) { rep_u32(rep, 0, MEM_ERR_BADSLOT); rep->length = 4; return MEM_ERR_BADSLOT; }
    SlotState *s = &mpstate.slots[slot];
    if (s->live_allocs == 0) { rep_u32(rep, 0, MEM_ERR_UNDERFLOW); rep->length = 4; return MEM_ERR_UNDERFLOW; }
    s->live_allocs--;
    if (s->live_bytes >= size) s->live_bytes -= size; else s->live_bytes = 0;
    s->total_frees++; s->total_free_bytes += size;
    if (s->live_allocs < s->prev_live_allocs) s->leak_alerted = false;
    rep_u32(rep, 0, MEM_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t handle_status(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t slot = msg_u32(req, 0);
    if (!valid_slot(slot)) { rep_u32(rep, 0, MEM_ERR_BADSLOT); rep->length = 4; return MEM_ERR_BADSLOT; }
    SlotState *s = &mpstate.slots[slot];
    rep_u32(rep, 0,  MEM_OK);
    rep_u32(rep, 4,  s->live_allocs);
    rep_u32(rep, 8,  (uint32_t)(s->live_bytes >> 32));
    rep_u32(rep, 12, (uint32_t)(s->live_bytes & 0xFFFFFFFF));
    rep_u32(rep, 16, (uint32_t)(s->total_allocs & 0xFFFFFFFF));
    rep_u32(rep, 20, (uint32_t)(s->total_frees  & 0xFFFFFFFF));
    rep_u32(rep, 24, s->monotonic_ticks);
    rep_u32(rep, 28, s->leak_alerted ? 1u : 0u);
    rep->length = 32;
    return SEL4_ERR_OK;
}

static uint32_t handle_snapshot(sel4_badge_t b, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    snapshot_all();
    rep_u32(rep, 0, MEM_OK);
    rep_u32(rep, 4, (uint32_t)(mpstate.ring_head & 0xFFFFFFFF));
    rep->length = 8;
    return SEL4_ERR_OK;
}

void mem_profiler_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    put_str("[mem_profiler] init — priority 108, passive\n");
    for (int i = 0; i < MAX_SLOTS; i++) {
        SlotState z = {0};
        mpstate.slots[i] = z;
    }
    mpstate.tick       = 0;
    mpstate.ring_head  = 0;
    mpstate.ring_ready = (mem_ring_vaddr != 0);
    if (mpstate.ring_ready) {
        put_str("[mem_profiler] ring mapped, 256KB\n");
    } else {
        put_str("[mem_profiler] WARNING: ring not mapped\n");
    }

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_MEM_REGISTER,   handle_register,  (void *)0);
    sel4_server_register(&srv, OP_MEM_ALLOC_HOOK,  handle_alloc,    (void *)0);
    sel4_server_register(&srv, OP_MEM_FREE_HOOK,   handle_free,     (void *)0);
    sel4_server_register(&srv, OP_MEM_STATUS,      handle_status,   (void *)0);
    sel4_server_register(&srv, OP_MEM_SNAPSHOT,    handle_snapshot, (void *)0);
    sel4_server_run(&srv);
}
