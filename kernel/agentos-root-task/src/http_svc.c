/*
 * http_svc.c — HTTP Service Protection Domain
 *
 * Active PD (priority 140, console slot 19).
 *
 * Maintains a table of up to HTTP_MAX_HANDLERS URL-prefix → app_id route
 * mappings.  AppManager registers routes here when launching apps and
 * unregisters them on teardown.  The Rust http-gateway calls OP_HTTP_DISPATCH
 * to find which app should handle an incoming request.
 *
 * URL prefixes are exchanged in message data bytes (8 bytes per 64-bit word,
 * words 4..11).  OP_HTTP_LIST writes the full handler table to http_req_shmem.
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC; microkit.h removed.
 * Prefix words now come from sel4_msg_t.data[].
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "agentos.h"
#include "sel4_server.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "http_svc.h"

/* ── Shared memory globals ────────────────────────────────────────────────── */
uintptr_t http_req_shmem_vaddr;
uintptr_t log_drain_rings_vaddr;

#define LOG(msg) log_drain_write(HTTP_SVC_CONSOLE_SLOT, HTTP_SVC_PD_ID, \
                             "[http_svc] " msg "\n")

/* ── Handler table ───────────────────────────────────────────────────────── */
typedef struct {
    bool     active;
    uint32_t handler_id;
    uint32_t app_id;
    uint32_t vnic_id;
    char     prefix[HTTP_PREFIX_MAX];
    uint32_t prefix_len;
} handler_entry_t;

static handler_entry_t handlers[HTTP_MAX_HANDLERS];
static uint32_t        next_handler_id = 0;

/* ── String helpers (no libc) ────────────────────────────────────────────── */

static uint32_t s_len(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

static void s_copy(char *dst, const char *src, uint32_t max) {
    uint32_t i;
    for (i = 0; i + 1 < max && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ── msg data helpers ────────────────────────────────────────────────────── */

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
static inline uint64_t msg_u64(const sel4_msg_t *m, uint32_t off) {
    return (uint64_t)msg_u32(m, off) | ((uint64_t)msg_u32(m, off + 4) << 32);
}
static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        m->data[off]=(uint8_t)v; m->data[off+1]=(uint8_t)(v>>8);
        m->data[off+2]=(uint8_t)(v>>16); m->data[off+3]=(uint8_t)(v>>24);
    }
}
#endif /* AGENTOS_IPC_HELPERS_DEFINED */

/*
 * unpack_prefix — extract the URL prefix from sel4_msg_t.data[].
 * Prefix words start at byte offset HTTP_PREFIX_MR_BASE * 4 in data[].
 * Each word is 4 bytes (uint32_t LE), and HTTP_PREFIX_MR_COUNT words total.
 * This gives HTTP_PREFIX_MR_COUNT*4 bytes = 32 bytes per word group.
 * HTTP_PREFIX_MR_BASE=4, HTTP_PREFIX_MR_COUNT=8 → data bytes 16..47.
 */
static void unpack_prefix(const sel4_msg_t *req, char *dst, uint32_t len) {
    uint32_t cap = len < HTTP_PREFIX_MAX ? len : HTTP_PREFIX_MAX - 1;
    /* Each "word" is one uint32 at 4-byte boundary in data[] */
    for (uint32_t w = 0; w < HTTP_PREFIX_MR_COUNT; w++) {
        uint32_t byte_off = (HTTP_PREFIX_MR_BASE + w) * 4u;
        uint32_t word = msg_u32(req, byte_off);
        for (uint32_t b = 0; b < 4; b++) {
            uint32_t idx = w * 4u + b;
            if (idx < cap)
                dst[idx] = (char)((word >> (b * 8u)) & 0xFF);
        }
    }
    dst[cap] = '\0';
}

/* ── Table helpers ───────────────────────────────────────────────────────── */

static handler_entry_t *alloc_handler(void) {
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++)
        if (!handlers[i].active) return &handlers[i];
    return (void *)0;
}

static handler_entry_t *find_by_handler_id(uint32_t hid) {
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++)
        if (handlers[i].active && handlers[i].handler_id == hid) return &handlers[i];
    return (void *)0;
}

static handler_entry_t *find_by_app_id(uint32_t app_id) {
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++)
        if (handlers[i].active && handlers[i].app_id == app_id) return &handlers[i];
    return (void *)0;
}

static uint32_t count_active(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++) n += handlers[i].active ? 1u : 0u;
    return n;
}

static handler_entry_t *longest_prefix_match(const char *path, uint32_t path_len) {
    handler_entry_t *best = (void *)0;
    uint32_t best_len = 0;
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++) {
        if (!handlers[i].active) continue;
        uint32_t plen = handlers[i].prefix_len;
        if (plen > path_len || plen < best_len) continue;
        bool match = true;
        for (uint32_t j = 0; j < plen; j++) {
            if (handlers[i].prefix[j] != path[j]) { match = false; break; }
        }
        if (match) { best = &handlers[i]; best_len = plen; }
    }
    return best;
}

/* ── Handlers ─────────────────────────────────────────────────────────────── */

static uint32_t h_register(sel4_badge_t b, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t app_id     = msg_u32(req, 4);
    uint32_t vnic_id    = msg_u32(req, 8);
    uint32_t prefix_len = msg_u32(req, 12);

    if (prefix_len == 0 || prefix_len > HTTP_PREFIX_MAX - 1) {
        rep_u32(rep, 0, HTTP_ERR_INVAL); rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }
    handler_entry_t *h = alloc_handler();
    if (!h) { rep_u32(rep, 0, HTTP_ERR_NO_SLOTS); rep->length = 4; return SEL4_ERR_NO_MEM; }

    unpack_prefix(req, h->prefix, prefix_len);
    h->prefix_len  = prefix_len;
    h->app_id      = app_id;
    h->vnic_id     = vnic_id;
    h->handler_id  = next_handler_id++;
    h->active      = true;
    LOG("route registered");

    rep_u32(rep, 0, HTTP_OK);
    rep_u32(rep, 4, h->handler_id);
    rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_unregister(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t id = msg_u32(req, 4);
    handler_entry_t *h = find_by_handler_id(id);
    if (!h) h = find_by_app_id(id);
    if (!h) { rep_u32(rep, 0, HTTP_ERR_NOT_FOUND); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    h->active = false;
    rep_u32(rep, 0, HTTP_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_dispatch(sel4_badge_t b, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    /* Path is packed in data[4..67] (64 bytes, 16 × uint32 words at byte offsets 4..67) */
    char path[65];
    path[64] = '\0';
    for (uint32_t w = 0; w < 16; w++) {
        uint32_t word = msg_u32(req, 4 + w * 4u);
        for (uint32_t bb = 0; bb < 4; bb++) {
            uint32_t idx = w * 4u + bb;
            if (idx < 64) path[idx] = (char)((word >> (bb * 8u)) & 0xFF);
        }
    }
    uint32_t path_len = 0;
    while (path_len < 64 && path[path_len]) path_len++;

    handler_entry_t *h = longest_prefix_match(path, path_len);
    rep_u32(rep, 0,  HTTP_OK);
    rep_u32(rep, 4,  h ? h->app_id     : HTTP_APP_ID_NONE);
    rep_u32(rep, 8,  h ? h->vnic_id    : 0xFFFFFFFFu);
    rep_u32(rep, 12, h ? h->handler_id : 0xFFFFFFFFu);
    rep->length = 16;
    return SEL4_ERR_OK;
}

static uint32_t h_list(sel4_badge_t b, const sel4_msg_t *req,
                         sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    if (!http_req_shmem_vaddr) {
        rep_u32(rep, 0, HTTP_ERR_INVAL); rep->length = 4; return SEL4_ERR_BAD_ARG;
    }
    volatile http_handler_entry_t *out =
        (volatile http_handler_entry_t *)http_req_shmem_vaddr;
    uint32_t count = 0;
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++) {
        if (!handlers[i].active) continue;
        volatile http_handler_entry_t *e = &out[count++];
        e->active     = true;
        e->_pad[0]    = 0; e->_pad[1] = 0; e->_pad[2] = 0;
        e->handler_id = handlers[i].handler_id;
        e->app_id     = handlers[i].app_id;
        e->vnic_id    = handlers[i].vnic_id;
        for (uint32_t j = 0; j < HTTP_PREFIX_MAX; j++)
            e->prefix[j] = handlers[i].prefix[j];
    }
    rep_u32(rep, 0, HTTP_OK);
    rep_u32(rep, 4, count);
    rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_health(sel4_badge_t b, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    rep_u32(rep, 0, HTTP_OK);
    rep_u32(rep, 4, count_active());
    rep_u32(rep, 8, HTTP_SVC_VERSION);
    rep->length = 12;
    return SEL4_ERR_OK;
}

void http_svc_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++) handlers[i].active = false;
    LOG("init complete");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_HTTP_REGISTER,   h_register,   (void *)0);
    sel4_server_register(&srv, OP_HTTP_UNREGISTER, h_unregister, (void *)0);
    sel4_server_register(&srv, OP_HTTP_DISPATCH,   h_dispatch,   (void *)0);
    sel4_server_register(&srv, OP_HTTP_LIST,       h_list,       (void *)0);
    sel4_server_register(&srv, OP_HTTP_HEALTH,     h_health,     (void *)0);
    sel4_server_run(&srv);
}
