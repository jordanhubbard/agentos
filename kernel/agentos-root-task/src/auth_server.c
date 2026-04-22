/*
 * auth_server.c — Authentication server for agentOS
 *
 * HURD-equivalent: auth server
 * Priority: 170 (passive, above most services)
 *
 * Provides user/group identity mapped to capability tokens.
 * Analogous to GNU HURD's auth server but using agentOS capability model.
 *
 * Channel assignments (auth_server's local view):
 *   id=0: receives PPC from controller (LOGIN/VERIFY/REVOKE/ADDUSER/STATUS)
 *
 * Pre-created users at init:
 *   uid=0 "root"  cap_mask=0xFF (all capabilities)
 *   uid=1 "admin" cap_mask=0x3F (no SWAP_WRITE/SWAP_READ caps)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdbool.h>

/* ── Configuration ─────────────────────────────────────────────────────── */
#define AUTH_MAX_USERS   16
#define AUTH_MAX_TOKENS  64
#define AUTH_TOKEN_MAGIC 0xA0710001u

/* ── Shared memory region for user name reads ─────────────────────────── */
uintptr_t auth_shmem_vaddr;   /* mapped by Microkit linker */

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
    uint64_t  issued_tick;  /* approximate (tick counter) */
    bool      valid;
} auth_token_t;

static auth_user_t  users[AUTH_MAX_USERS];
static auth_token_t tokens[AUTH_MAX_TOKENS];
static uint32_t     next_token_id = 1;
static uint64_t     tick_counter  = 0;

/* ── Channel IDs ───────────────────────────────────────────────────────── */
#define CH_CTRL_PPC  0

/* ── Helpers ───────────────────────────────────────────────────────────── */
static auth_user_t *find_user(uint32_t uid) {
    for (int i = 0; i < AUTH_MAX_USERS; i++)
        if (users[i].active && users[i].uid == uid) return &users[i];
    return NULL;
}

static auth_token_t *find_token(uint32_t token_id) {
    for (int i = 0; i < AUTH_MAX_TOKENS; i++)
        if (tokens[i].valid && tokens[i].token_id == token_id) return &tokens[i];
    return NULL;
}

static auth_token_t *alloc_token(void) {
    for (int i = 0; i < AUTH_MAX_TOKENS; i++)
        if (!tokens[i].valid) return &tokens[i];
    return NULL;
}

/* ── Microkit entry points ─────────────────────────────────────────────── */

void init(void)
{
    /* Clear all state */
    for (int i = 0; i < AUTH_MAX_USERS;  i++) users[i].active  = false;
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) tokens[i].valid  = false;

    /* Pre-create root (uid=0) */
    users[0].uid      = 0;
    users[0].cap_mask = 0xFFu;
    users[0].active   = true;
    users[0].name[0]  = 'r'; users[0].name[1] = 'o';
    users[0].name[2]  = 'o'; users[0].name[3] = 't';
    users[0].name[4]  = '\0';

    /* Pre-create admin (uid=1) */
    users[1].uid      = 1;
    users[1].cap_mask = 0x3Fu;   /* COMPUTE|MEM|OBJECTSTORE|NETWORK|SPAWN|AUDIT */
    users[1].active   = true;
    users[1].name[0]  = 'a'; users[1].name[1] = 'd';
    users[1].name[2]  = 'm'; users[1].name[3] = 'i';
    users[1].name[4]  = 'n'; users[1].name[5] = '\0';

    microkit_dbg_puts("[auth_server] READY: 2 users, root+admin\n");
}

void notified(microkit_channel ch) { (void)ch; tick_counter++; }

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg)
{
    (void)ch; (void)msg;
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {

    /* OP_AUTH_LOGIN: MR1=uid → MR0=ok, MR1=token_id */
    case OP_AUTH_LOGIN: {
        uint32_t uid = (uint32_t)microkit_mr_get(1);
        auth_user_t *u = find_user(uid);
        if (!u) {
            microkit_mr_set(0, 0xFFu);  /* AUTH_ERR_NO_USER */
            return microkit_msginfo_new(0, 1);
        }
        auth_token_t *t = alloc_token();
        if (!t) {
            microkit_mr_set(0, 0xFEu);  /* AUTH_ERR_NO_TOKENS */
            return microkit_msginfo_new(0, 1);
        }
        t->token_id    = next_token_id++;
        t->uid         = uid;
        t->cap_mask    = u->cap_mask;
        t->issued_tick = tick_counter;
        t->valid       = true;
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, t->token_id);
        return microkit_msginfo_new(0, 2);
    }

    /* OP_AUTH_VERIFY: MR1=token_id → MR0=ok, MR1=uid, MR2=cap_mask */
    case OP_AUTH_VERIFY: {
        uint32_t token_id = (uint32_t)microkit_mr_get(1);
        auth_token_t *t = find_token(token_id);
        if (!t) {
            microkit_mr_set(0, 0xFFu);  /* AUTH_ERR_BAD_TOKEN */
            return microkit_msginfo_new(0, 1);
        }
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, t->uid);
        microkit_mr_set(2, t->cap_mask);
        return microkit_msginfo_new(0, 3);
    }

    /* OP_AUTH_REVOKE: MR1=token_id → MR0=ok */
    case OP_AUTH_REVOKE: {
        uint32_t token_id = (uint32_t)microkit_mr_get(1);
        auth_token_t *t = find_token(token_id);
        if (t) t->valid = false;
        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    /* OP_AUTH_ADDUSER: MR1=uid, MR2=cap_mask → MR0=ok */
    case OP_AUTH_ADDUSER: {
        uint32_t uid      = (uint32_t)microkit_mr_get(1);
        uint32_t cap_mask = (uint32_t)microkit_mr_get(2);
        /* Check uid not already active */
        if (find_user(uid)) {
            microkit_mr_set(0, 0xFDu);  /* AUTH_ERR_EXISTS */
            return microkit_msginfo_new(0, 1);
        }
        /* Find free user slot */
        auth_user_t *slot = NULL;
        for (int i = 0; i < AUTH_MAX_USERS; i++)
            if (!users[i].active) { slot = &users[i]; break; }
        if (!slot) {
            microkit_mr_set(0, 0xFCu);  /* AUTH_ERR_FULL */
            return microkit_msginfo_new(0, 1);
        }
        slot->uid      = uid;
        slot->cap_mask = cap_mask;
        slot->active   = true;
        /* Read name from auth_shmem if mapped */
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
        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    /* OP_AUTH_STATUS: → MR0=ok, MR1=active_tokens, MR2=active_users */
    case OP_AUTH_STATUS: {
        uint32_t atokens = 0, ausers = 0;
        for (int i = 0; i < AUTH_MAX_TOKENS; i++) if (tokens[i].valid)  atokens++;
        for (int i = 0; i < AUTH_MAX_USERS;  i++) if (users[i].active)  ausers++;
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, atokens);
        microkit_mr_set(2, ausers);
        return microkit_msginfo_new(0, 3);
    }

    default:
        microkit_mr_set(0, 0xFFu);
        return microkit_msginfo_new(0, 1);
    }
}
