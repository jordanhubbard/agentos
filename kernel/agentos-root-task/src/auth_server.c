/*
 * auth_server.c — Authentication server for agentOS
 *
 * HURD-equivalent: auth server
 * Priority: 170 (passive, above most services)
 *
 * Provides user/group identity mapped to capability tokens.
 * Analogous to GNU HURD's auth server but using agentOS capability model.
 *
 * Pre-created users at init:
 *   uid=0 "root"  cap_mask=0xFF (all capabilities)
 *   uid=1 "admin" cap_mask=0x3F (no SWAP_WRITE/SWAP_READ caps)
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include <stdbool.h>

/* ── Configuration ─────────────────────────────────────────────────────── */
#define AUTH_MAX_USERS   16
#define AUTH_MAX_TOKENS  64
#define AUTH_TOKEN_MAGIC 0xA0710001u

/* ── Shared memory region for user name reads ─────────────────────────── */
uintptr_t auth_shmem_vaddr;

/* ── Data structures ───────────────────────────────────────────────────── */
typedef struct {
    uint32_t uid;
    char     name[16];
    uint32_t cap_mask;
    bool     active;
} auth_user_t;

typedef struct {
    uint32_t  token_id;
    uint32_t  uid;
    uint32_t  cap_mask;
    uint64_t  issued_tick;
    bool      valid;
} auth_token_t;

static auth_user_t  users[AUTH_MAX_USERS];
static auth_token_t tokens[AUTH_MAX_TOKENS];
static uint32_t     next_token_id = 1;
static uint64_t     tick_counter  = 0;

/* ── Helpers ───────────────────────────────────────────────────────────── */
static auth_user_t *find_user(uint32_t uid) {
    for (int i = 0; i < AUTH_MAX_USERS; i++)
        if (users[i].active && users[i].uid == uid) return &users[i];
    return (void *)0;
}

static auth_token_t *find_token(uint32_t token_id) {
    for (int i = 0; i < AUTH_MAX_TOKENS; i++)
        if (tokens[i].valid && tokens[i].token_id == token_id) return &tokens[i];
    return (void *)0;
}

static auth_token_t *alloc_token(void) {
    for (int i = 0; i < AUTH_MAX_TOKENS; i++)
        if (!tokens[i].valid) return &tokens[i];
    return (void *)0;
}

/* ── Helpers to read/write msg data fields ─────────────────────────────── */
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
        m->data[off]   = (uint8_t)(v);
        m->data[off+1] = (uint8_t)(v >> 8);
        m->data[off+2] = (uint8_t)(v >> 16);
        m->data[off+3] = (uint8_t)(v >> 24);
    }
}

/* ── Opcode handlers ───────────────────────────────────────────────────── */

static uint32_t handle_login(sel4_badge_t b, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint32_t uid = msg_u32(req, 0);
    auth_user_t *u = find_user(uid);
    if (!u) { rep_u32(rep, 0, 0xFFu); rep->length = 4; return 0xFFu; }
    auth_token_t *t = alloc_token();
    if (!t) { rep_u32(rep, 0, 0xFEu); rep->length = 4; return 0xFEu; }
    t->token_id    = next_token_id++;
    t->uid         = uid;
    t->cap_mask    = u->cap_mask;
    t->issued_tick = tick_counter;
    t->valid       = true;
    rep_u32(rep, 0, 0u);
    rep_u32(rep, 4, t->token_id);
    rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t handle_verify(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint32_t token_id = msg_u32(req, 0);
    auth_token_t *t = find_token(token_id);
    if (!t) { rep_u32(rep, 0, 0xFFu); rep->length = 4; return 0xFFu; }
    rep_u32(rep, 0, 0u);
    rep_u32(rep, 4, t->uid);
    rep_u32(rep, 8, t->cap_mask);
    rep->length = 12;
    return SEL4_ERR_OK;
}

static uint32_t handle_revoke(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint32_t token_id = msg_u32(req, 0);
    auth_token_t *t = find_token(token_id);
    if (t) t->valid = false;
    rep_u32(rep, 0, 0u);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t handle_adduser(sel4_badge_t b, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint32_t uid      = msg_u32(req, 0);
    uint32_t cap_mask = msg_u32(req, 4);
    if (find_user(uid)) { rep_u32(rep, 0, 0xFDu); rep->length = 4; return 0xFDu; }
    auth_user_t *slot = (void *)0;
    for (int i = 0; i < AUTH_MAX_USERS; i++)
        if (!users[i].active) { slot = &users[i]; break; }
    if (!slot) { rep_u32(rep, 0, 0xFCu); rep->length = 4; return 0xFCu; }
    slot->uid      = uid;
    slot->cap_mask = cap_mask;
    slot->active   = true;
    if (auth_shmem_vaddr) {
        const char *name = (const char *)(uintptr_t)auth_shmem_vaddr;
        for (int i = 0; i < 15; i++) {
            slot->name[i] = name[i];
            if (!name[i]) break;
        }
        slot->name[15] = '\0';
    } else {
        slot->name[0] = 'u'; slot->name[1] = '\0';
    }
    rep_u32(rep, 0, 0u);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t handle_status(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)req; (void)ctx;
    uint32_t atokens = 0, ausers = 0;
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) if (tokens[i].valid)  atokens++;
    for (int i = 0; i < AUTH_MAX_USERS;  i++) if (users[i].active)  ausers++;
    rep_u32(rep, 0, 0u);
    rep_u32(rep, 4, atokens);
    rep_u32(rep, 8, ausers);
    rep->length = 12;
    return SEL4_ERR_OK;
}

/* ── Main entry point ──────────────────────────────────────────────────── */

void auth_server_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;

    /* Initialise state */
    for (int i = 0; i < AUTH_MAX_USERS;  i++) users[i].active  = false;
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) tokens[i].valid  = false;

    /* Pre-create root (uid=0) */
    users[0].uid      = 0;
    users[0].cap_mask = 0xFFu;
    users[0].active   = true;
    users[0].name[0]  = 'r'; users[0].name[1] = 'o';
    users[0].name[2]  = 'o'; users[0].name[3] = 't'; users[0].name[4] = '\0';

    /* Pre-create admin (uid=1) */
    users[1].uid      = 1;
    users[1].cap_mask = 0x3Fu;
    users[1].active   = true;
    users[1].name[0]  = 'a'; users[1].name[1] = 'd';
    users[1].name[2]  = 'm'; users[1].name[3] = 'i';
    users[1].name[4]  = 'n'; users[1].name[5] = '\0';

    sel4_dbg_puts("[auth_server] READY: 2 users, root+admin\n");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_AUTH_LOGIN,   handle_login,   (void *)0);
    sel4_server_register(&srv, OP_AUTH_VERIFY,  handle_verify,  (void *)0);
    sel4_server_register(&srv, OP_AUTH_REVOKE,  handle_revoke,  (void *)0);
    sel4_server_register(&srv, OP_AUTH_ADDUSER, handle_adduser, (void *)0);
    sel4_server_register(&srv, OP_AUTH_STATUS,  handle_status,  (void *)0);
    sel4_server_run(&srv);
}

/* suppress tick_counter unused warning */
static void _tick(void) { tick_counter++; }
static void (*_tick_fn)(void) __attribute__((unused)) = _tick;
