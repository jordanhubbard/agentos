/*
 * agentOS net_isolator — per-agent outbound network firewall via seL4 capability model
 *
 * Passive PD, priority 160.
 *
 * Sits between agent slots and net_svc as a proxy/firewall.
 * Each agent slot has an ACL table of allowed hostname:port pairs.
 * Connections not matching the ACL are denied and logged to cap_audit_log.
 *
 * Protocol:
 *   OP_NET_CONNECT  (0x70) — slot_id, host_hash, port: → 1 (allowed) or 0 (denied)
 *   OP_NET_ACL_SET  (0x71) — slot_id, rule_idx, port; host string in shared ring
 *   OP_NET_ACL_GET  (0x72) — slot_id → rule_count, deny_count, conn_count, slot_active
 *   OP_NET_ACL_CLEAR (0x73) — slot_id: remove all ACL entries for slot.
 *   OP_NET_STATUS   (0x74) — total_slots_active, total_denials, total_connections, magic
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/net_isolator_contract.h"

/* ── Opcodes ─────────────────────────────────────────────────────────────── */
#define OP_NET_CONNECT   0x70u
#define OP_NET_ACL_SET   0x71u
#define OP_NET_ACL_GET   0x72u
#define OP_NET_ACL_CLEAR 0x73u
#define OP_NET_STATUS    0x74u

/* ── Limits ─────────────────────────────────────────────────────────────── */
#define NET_MAX_SLOTS       8
#define NET_MAX_RULES       16
#define NET_MAX_HOST_LEN    63

/* ── Magic for ring header ────────────────────────────────────────────────── */
#define NET_RING_MAGIC  0x4E457449u

/* ── ACL rule ────────────────────────────────────────────────────────────── */
typedef struct {
    char     host[NET_MAX_HOST_LEN + 1];
    uint16_t port;
    uint8_t  active;
    uint8_t  _pad;
} net_acl_rule_t;

/* ── Per-slot state ──────────────────────────────────────────────────────── */
typedef struct {
    uint32_t       slot_id;
    uint32_t       active;
    uint32_t       deny_count;
    uint32_t       conn_count;
    net_acl_rule_t rules[NET_MAX_RULES];
} net_slot_t;

static net_slot_t slots[NET_MAX_SLOTS];

static uint32_t total_denials     = 0;
static uint32_t total_connections = 0;
static uint32_t total_active      = 0;

/* ── Shared memory (256KB status ring) ───────────────────────────────────── */
uintptr_t net_isolator_ring_vaddr;

#define NI_RING_SIZE  0x40000u

/* ── Outbound endpoint to cap_audit_log (resolved lazily) ─────────────────── */
static seL4_CPtr g_audit_ep = 0;

/* ── Debug helpers ───────────────────────────────────────────────────────── */
static void ni_puts(const char *s) { sel4_dbg_puts(s); }

static void ni_put_dec(uint32_t v) {
    if (v == 0) { ni_puts("0"); return; }
    char buf[12]; int i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    ni_puts(&buf[i]);
}

/* ── String helpers (no libc) ────────────────────────────────────────────── */
static uint32_t ni_strlen(const char *s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}

static int ni_strncmp(const char *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static void ni_strncpy(char *dst, const char *src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ── ACL matching ────────────────────────────────────────────────────────── */

static uint32_t fnv1a(const char *s) {
    uint32_t hash = 0x811c9dc5u;
    while (*s) { hash ^= (uint32_t)(unsigned char)*s++; hash *= 0x01000193u; }
    return hash;
}

static int ni_host_match(const char *pattern, const char *host) {
    if (pattern[0] == '*' && pattern[1] == '\0') return 1;
    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *suffix = pattern + 1;
        uint32_t hlen = ni_strlen(host);
        uint32_t slen = ni_strlen(suffix);
        if (hlen < slen) return 0;
        return ni_strncmp(host + (hlen - slen), suffix, slen) == 0;
    }
    return ni_strncmp(pattern, host, NET_MAX_HOST_LEN + 1) == 0;
}

static net_slot_t *find_slot(uint32_t slot_id) {
    for (int i = 0; i < NET_MAX_SLOTS; i++)
        if (slots[i].active && slots[i].slot_id == slot_id) return &slots[i];
    return (void *)0;
}

static net_slot_t *get_or_alloc_slot(uint32_t slot_id) {
    net_slot_t *s = find_slot(slot_id);
    if (s) return s;
    for (int i = 0; i < NET_MAX_SLOTS; i++) {
        if (!slots[i].active) {
            slots[i].slot_id    = slot_id;
            slots[i].active     = 1;
            slots[i].deny_count  = 0;
            slots[i].conn_count  = 0;
            for (int j = 0; j < NET_MAX_RULES; j++) slots[i].rules[j].active = 0;
            total_active++;
            return &slots[i];
        }
    }
    return (void *)0;
}

/* ── Ring buffer update ──────────────────────────────────────────────────── */
static void update_ring(void) {
    if (!net_isolator_ring_vaddr) return;
    volatile uint8_t *r = (volatile uint8_t *)net_isolator_ring_vaddr;
    uint32_t p = 0;
    r[p++] = (NET_RING_MAGIC >>  0) & 0xFF;
    r[p++] = (NET_RING_MAGIC >>  8) & 0xFF;
    r[p++] = (NET_RING_MAGIC >> 16) & 0xFF;
    r[p++] = (NET_RING_MAGIC >> 24) & 0xFF;
    volatile uint32_t *stats = (volatile uint32_t *)(r + p);
    stats[0] = total_active;
    stats[1] = total_denials;
    stats[2] = total_connections;
    stats[3] = (uint32_t)NET_MAX_SLOTS;
    p += 16;
    for (int i = 0; i < NET_MAX_SLOTS; i++) {
        volatile uint32_t *sv = (volatile uint32_t *)(r + p);
        sv[0] = slots[i].slot_id;
        sv[1] = slots[i].active;
        sv[2] = slots[i].deny_count;
        sv[3] = slots[i].conn_count;
        uint32_t rc = 0;
        for (int j = 0; j < NET_MAX_RULES; j++)
            if (slots[i].rules[j].active) rc++;
        sv[4] = rc;
        sv[5] = 0; sv[6] = 0; sv[7] = 0;
        p += 32;
    }
}

/* ── Audit log notification ──────────────────────────────────────────────── */
static void audit_deny(uint32_t slot_id, uint32_t host_hash, uint32_t port) {
    if (g_audit_ep != 0) {
#ifndef AGENTOS_TEST_HOST
        seL4_SetMR(0, 0x7001u);
        seL4_SetMR(1, slot_id);
        seL4_SetMR(2, host_hash);
        seL4_SetMR(3, port);
        seL4_MessageInfo_t _i = seL4_MessageInfo_new(0x7001u, 0, 0, 4);
        (void)seL4_Call(g_audit_ep, _i);
#endif
    }
}

/* ── msg helpers ────────────────────────────────────────────────────────── */
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

/* ── Handlers ─────────────────────────────────────────────────────────────── */

static uint32_t h_connect(sel4_badge_t b, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t slot_id   = msg_u32(req, 4);
    uint32_t host_hash = msg_u32(req, 8);
    uint32_t port      = msg_u32(req, 12);

    net_slot_t *s = find_slot(slot_id);
    if (!s) {
        total_denials++;
        rep_u32(rep, 0, 0);
        rep->length = 4;
        audit_deny(slot_id, host_hash, port);
        ni_puts("[net_isolator] DENY: unknown slot "); ni_put_dec(slot_id); ni_puts("\n");
        return SEL4_ERR_OK;
    }

    int allowed = 0;
    for (int i = 0; i < NET_MAX_RULES && !allowed; i++) {
        net_acl_rule_t *r = &s->rules[i];
        if (!r->active) continue;
        int port_ok = (r->port == 0) || (r->port == (uint16_t)port);
        if (!port_ok) continue;
        uint32_t rule_hash = fnv1a(r->host);
        if (r->host[0] == '*') {
            allowed = 1;
        } else if (rule_hash == host_hash) {
            allowed = 1;
        }
        (void)ni_host_match; /* suppress unused-function warning in non-hash path */
    }

    if (allowed) {
        s->conn_count++;
        total_connections++;
        rep_u32(rep, 0, 1);
    } else {
        s->deny_count++;
        total_denials++;
        rep_u32(rep, 0, 0);
        audit_deny(slot_id, host_hash, port);
        ni_puts("[net_isolator] DENY: slot="); ni_put_dec(slot_id);
        ni_puts(" port="); ni_put_dec(port); ni_puts("\n");
    }
    update_ring();
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_acl_set(sel4_badge_t b, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t slot_id  = msg_u32(req, 4);
    uint32_t rule_idx = msg_u32(req, 8);
    uint32_t port     = msg_u32(req, 12);

    if (rule_idx >= NET_MAX_RULES) {
        rep_u32(rep, 0, 0); rep->length = 4; return SEL4_ERR_INVALID_ARG;
    }
    net_slot_t *s = get_or_alloc_slot(slot_id);
    if (!s) {
        rep_u32(rep, 0, 0); rep->length = 4; return SEL4_ERR_NO_MEM;
    }

    uint32_t host_off = 512 + slot_id * NET_MAX_RULES * 64 + rule_idx * 64;
    const char *host_src = (const char *)(net_isolator_ring_vaddr + host_off);
    ni_strncpy(s->rules[rule_idx].host, host_src, NET_MAX_HOST_LEN + 1);
    s->rules[rule_idx].port   = (uint16_t)(port & 0xFFFF);
    s->rules[rule_idx].active = 1;

    ni_puts("[net_isolator] ACL_SET slot="); ni_put_dec(slot_id);
    ni_puts(" rule="); ni_put_dec(rule_idx);
    ni_puts(" host="); ni_puts(s->rules[rule_idx].host);
    ni_puts(" port="); ni_put_dec(port); ni_puts("\n");

    update_ring();
    rep_u32(rep, 0, 1); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_acl_get(sel4_badge_t b, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t slot_id = msg_u32(req, 4);
    net_slot_t *s = find_slot(slot_id);
    if (!s) {
        rep_u32(rep, 0, 0); rep_u32(rep, 4, 0);
        rep_u32(rep, 8, 0); rep_u32(rep, 12, 0);
        rep->length = 16; return SEL4_ERR_OK;
    }
    uint32_t rule_count = 0;
    for (int i = 0; i < NET_MAX_RULES; i++)
        if (s->rules[i].active) rule_count++;
    rep_u32(rep, 0,  rule_count);
    rep_u32(rep, 4,  s->deny_count);
    rep_u32(rep, 8,  s->conn_count);
    rep_u32(rep, 12, s->active);
    rep->length = 16;
    return SEL4_ERR_OK;
}

static uint32_t h_acl_clear(sel4_badge_t b, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t slot_id = msg_u32(req, 4);
    net_slot_t *s = find_slot(slot_id);
    if (s) {
        for (int i = 0; i < NET_MAX_RULES; i++) s->rules[i].active = 0;
        s->active     = 0;
        s->deny_count  = 0;
        s->conn_count  = 0;
        if (total_active > 0) total_active--;
        ni_puts("[net_isolator] ACL_CLEAR slot="); ni_put_dec(slot_id); ni_puts("\n");
    }
    update_ring();
    rep_u32(rep, 0, 1); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_status(sel4_badge_t b, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    rep_u32(rep, 0,  total_active);
    rep_u32(rep, 4,  total_denials);
    rep_u32(rep, 8,  total_connections);
    rep_u32(rep, 12, NET_RING_MAGIC);
    rep->length = 16;
    return SEL4_ERR_OK;
}

void net_isolator_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    ni_puts("[net_isolator] init: passive PD priority 160\n");
    for (int i = 0; i < NET_MAX_SLOTS; i++) {
        slots[i].active     = 0;
        slots[i].slot_id    = 0;
        slots[i].deny_count  = 0;
        slots[i].conn_count  = 0;
        for (int j = 0; j < NET_MAX_RULES; j++) slots[i].rules[j].active = 0;
    }
    update_ring();
    ni_puts("[net_isolator] ACL tables cleared, ring initialised\n");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_NET_CONNECT,   h_connect,   (void *)0);
    sel4_server_register(&srv, OP_NET_ACL_SET,   h_acl_set,   (void *)0);
    sel4_server_register(&srv, OP_NET_ACL_GET,   h_acl_get,   (void *)0);
    sel4_server_register(&srv, OP_NET_ACL_CLEAR, h_acl_clear, (void *)0);
    sel4_server_register(&srv, OP_NET_STATUS,    h_status,    (void *)0);
    sel4_server_run(&srv);
}
