/*
 * SpawnServer IPC Contract
 *
 * SpawnServer (active PD, priority 120) dynamically launches application PDs
 * by assigning pre-allocated "app slot" protection domains and loading ELF
 * images into them via shared memory.
 *
 * Channel: CH_SPAWN_SERVER (20) — controller PPCs into spawn_server.
 *
 * Shared memory:
 *   spawn_config_shmem (4KB) — caller writes a NUL-terminated app name and
 *   ELF path (separated by a NUL byte) before calling OP_SPAWN_LAUNCH.
 *   An optional spawn_list_shmem region holds app_slot_info_t[] after
 *   OP_SPAWN_LIST.
 *
 * Invariants:
 *   - spawn_config_shmem must be populated before OP_SPAWN_LAUNCH is called.
 *   - app_id values are assigned by the server and are opaque to callers.
 *   - OP_SPAWN_STATUS with app_id == 0xFFFFFFFF returns aggregate statistics
 *     (total slots, busy slots) in MR1 and MR2.
 *   - SPAWN_VERSION is returned in MR2 of the OP_SPAWN_HEALTH reply.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define SPAWN_CH_CONTROLLER   CH_SPAWN_SERVER   /* controller → spawn_server */

/* ─── Version ────────────────────────────────────────────────────────────── */
#define SPAWN_VERSION   1u

/* ─── Opcodes (placed in MR0) ────────────────────────────────────────────── */
#define OP_SPAWN_LAUNCH   0x01u  /* launch a new application into a free slot */
#define OP_SPAWN_KILL     0x02u  /* kill a running application by app_id */
#define OP_SPAWN_STATUS   0x03u  /* query state of one app (or aggregate) */
#define OP_SPAWN_LIST     0x04u  /* list all slots (results in shmem) */
#define OP_SPAWN_HEALTH   0x05u  /* return free_slots + SPAWN_VERSION */

/* ─── Request structs ────────────────────────────────────────────────────── */

/* OP_SPAWN_LAUNCH
 * MR0=op, MR1=cap_classes   (name+elf_path already in spawn_config_shmem)
 */
struct __attribute__((packed)) spawn_server_req_launch {
    uint32_t op;          /* OP_SPAWN_LAUNCH */
    uint32_t cap_classes; /* capability class bitmask granted to the new PD */
};

/* OP_SPAWN_KILL
 * MR0=op, MR1=app_id
 */
struct __attribute__((packed)) spawn_server_req_kill {
    uint32_t op;     /* OP_SPAWN_KILL */
    uint32_t app_id;
};

/* OP_SPAWN_STATUS
 * MR0=op, MR1=app_id  (0xFFFFFFFF = aggregate statistics)
 */
struct __attribute__((packed)) spawn_server_req_status {
    uint32_t op;     /* OP_SPAWN_STATUS */
    uint32_t app_id; /* specific app or 0xFFFFFFFF for aggregate */
};

/* OP_SPAWN_LIST
 * MR0=op  (results written into spawn_list_shmem)
 */
struct __attribute__((packed)) spawn_server_req_list {
    uint32_t op;     /* OP_SPAWN_LIST */
};

/* OP_SPAWN_HEALTH
 * MR0=op
 */
struct __attribute__((packed)) spawn_server_req_health {
    uint32_t op;     /* OP_SPAWN_HEALTH */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

/* OP_SPAWN_LAUNCH reply: MR0=ok, MR1=app_id, MR2=slot_index */
struct __attribute__((packed)) spawn_server_reply_launch {
    uint32_t ok;         /* SPAWN_OK or spawn_error */
    uint32_t app_id;     /* assigned application identifier */
    uint32_t slot_index; /* which app_slot PD is hosting this application */
};

/* OP_SPAWN_KILL reply: MR0=ok */
struct __attribute__((packed)) spawn_server_reply_kill {
    uint32_t ok;         /* SPAWN_OK or spawn_error */
};

/* OP_SPAWN_STATUS reply: MR0=ok, MR1=state, MR2=cap_classes */
struct __attribute__((packed)) spawn_server_reply_status {
    uint32_t ok;         /* SPAWN_OK or spawn_error */
    uint32_t state;      /* application lifecycle state (0=FREE 1=LOADING 2=RUNNING 3=KILLED) */
    uint32_t cap_classes;
};

/* OP_SPAWN_LIST reply: MR0=ok, MR1=count */
struct __attribute__((packed)) spawn_server_reply_list {
    uint32_t ok;         /* SPAWN_OK or spawn_error */
    uint32_t count;      /* number of app_slot_info_t entries written to shmem */
};

/* OP_SPAWN_HEALTH reply: MR0=ok, MR1=free_slots, MR2=SPAWN_VERSION */
struct __attribute__((packed)) spawn_server_reply_health {
    uint32_t ok;           /* SPAWN_OK */
    uint32_t free_slots;
    uint32_t spawn_version; /* SPAWN_VERSION constant */
};

/* ─── Shmem layout: slot info entry (OP_SPAWN_LIST) ─────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t app_id;
    uint32_t slot_index;
    uint32_t state;
    uint32_t cap_classes;
    uint8_t  name[32];   /* NUL-terminated application name */
} spawn_slot_info_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum spawn_server_error {
    SPAWN_OK           = 0,
    SPAWN_ERR_INVAL    = 1,
    SPAWN_ERR_NO_SLOTS = 2,
    SPAWN_ERR_VFS      = 3,
    SPAWN_ERR_NOT_FOUND= 4,
    SPAWN_ERR_TOO_LARGE= 5,
};
