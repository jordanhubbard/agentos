/*
 * AuthServer IPC Contract
 *
 * The AuthServer PD is the single authentication authority for agentOS.
 * It issues opaque token IDs on successful login, which other PDs present
 * when invoking capability-guarded operations.  Token-to-capability mappings
 * are authoritative: every system service that checks caller identity must
 * call OP_AUTH_VERIFY rather than cache tokens locally.
 *
 * Channel: CH_AUTH_SERVER = 29 (from controller perspective)
 * Opcodes: OP_AUTH_LOGIN, OP_AUTH_VERIFY, OP_AUTH_REVOKE,
 *          OP_AUTH_ADDUSER, OP_AUTH_STATUS
 *
 * Invariants:
 *   - User name for ADDUSER is a NUL-terminated string placed in shared
 *     memory before the IPC call.
 *   - Tokens are opaque uint32_t values; the AuthServer owns the namespace.
 *   - A token is valid until explicitly revoked or the AuthServer reboots.
 *   - ADDUSER requires the caller to hold AGENTOS_CAP_ADMIN.
 *   - There is no persistent credential store in this revision; user records
 *     are RAM-only and lost on reboot.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define CH_AUTH_SERVER   29      /* controller → auth_server */

/* ─── Opcodes ─────────────────────────────────────────────────────────────── */
#define OP_AUTH_LOGIN    0x01
#define OP_AUTH_VERIFY   0x02
#define OP_AUTH_REVOKE   0x03
#define OP_AUTH_ADDUSER  0x04
#define OP_AUTH_STATUS   0x05

/* ─── Request structs ────────────────────────────────────────────────────── */

struct auth_server_req_login {
    uint32_t op;                 /* OP_AUTH_LOGIN */
    uint32_t uid;                /* numeric user identifier */
};

struct auth_server_req_verify {
    uint32_t op;                 /* OP_AUTH_VERIFY */
    uint32_t token_id;           /* token to verify */
};

struct auth_server_req_revoke {
    uint32_t op;                 /* OP_AUTH_REVOKE */
    uint32_t token_id;           /* token to invalidate */
};

struct auth_server_req_adduser {
    uint32_t op;                 /* OP_AUTH_ADDUSER */
    uint32_t uid;                /* numeric user identifier */
    uint32_t cap_mask;           /* AGENTOS_CAP_* bitmask for new user */
    /* NUL-terminated name string in shmem */
};

struct auth_server_req_status {
    uint32_t op;                 /* OP_AUTH_STATUS */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct auth_server_reply_login {
    uint32_t ok;                 /* AUTH_OK or error code */
    uint32_t token_id;           /* issued token; 0 on error */
};

struct auth_server_reply_verify {
    uint32_t ok;                 /* AUTH_OK or error code */
    uint32_t uid;                /* uid the token belongs to */
    uint32_t cap_mask;           /* AGENTOS_CAP_* bitmask for this token */
};

struct auth_server_reply_revoke {
    uint32_t ok;                 /* AUTH_OK or error code */
};

struct auth_server_reply_adduser {
    uint32_t ok;                 /* AUTH_OK or error code */
};

struct auth_server_reply_status {
    uint32_t ok;                 /* AUTH_OK or error code */
    uint32_t active_tokens;      /* number of live tokens */
    uint32_t active_users;       /* number of registered users */
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum auth_server_error {
    AUTH_OK             = 0x00,
    AUTH_ERR_FULL       = 0xFC,  /* token or user table is full */
    AUTH_ERR_EXISTS     = 0xFD,  /* user already registered */
    AUTH_ERR_NOTOKENS   = 0xFE,  /* token not found / invalid */
    AUTH_ERR_NOUSER     = 0xFF,  /* uid not found */
};
