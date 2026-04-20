/*
 * AppManager IPC Contract
 *
 * app_manager is an active seL4 protection domain (priority 130) that
 * orchestrates full-stack application deployment.  It reads key=value
 * manifests from app_manifest_shmem, allocates a virtual NIC via NetServer,
 * delegates ELF loading to SpawnServer, and registers HTTP route prefixes
 * with http_svc.  On kill it reverses all of these steps.
 *
 * Channel: APP_CH_CONTROLLER (0) — controller PPCs into app_manager.
 *
 * Shared memory:
 *   app_manifest_shmem — caller writes a NUL-terminated key=value manifest
 *                        before calling OP_APP_LAUNCH.  Format:
 *                          name=<app>\0elf=<path>\0url=<prefix>\0\0
 *   app_list_shmem     — written by app_manager on OP_APP_LIST with
 *                        app_info_t[] entries (count returned in MR1).
 *
 * Invariants:
 *   - manifest_len in OP_APP_LAUNCH is the total byte length of the
 *     NUL-terminated manifest string including all NUL separators.
 *   - app_id is an opaque 32-bit value assigned by app_manager at launch.
 *   - vnic_id is the virtual NIC handle returned by NetServer; 0 if no NIC
 *     could be allocated.
 *   - spawn_slot is the SpawnServer slot index hosting the application.
 *   - APP_MANAGER_VERSION is returned in MR2 of the OP_APP_HEALTH reply.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define APP_CH_CONTROLLER   0u   /* controller → app_manager (from app_manager's perspective) */

/* ─── Version ────────────────────────────────────────────────────────────── */
#define APP_MANAGER_VERSION   1u

/* ─── Opcodes (placed in MR0) ────────────────────────────────────────────── */
#define OP_APP_LAUNCH   0x01u  /* deploy a new application from manifest in shmem */
#define OP_APP_KILL     0x02u  /* tear down a running application */
#define OP_APP_STATUS   0x03u  /* query state of an application */
#define OP_APP_LIST     0x04u  /* list all applications (results in shmem) */
#define OP_APP_HEALTH   0x05u  /* return running_count + APP_MANAGER_VERSION */

/* ─── Request structs ────────────────────────────────────────────────────── */

/* OP_APP_LAUNCH
 * MR0=op, MR1=manifest_len   (manifest already in app_manifest_shmem)
 */
struct __attribute__((packed)) app_manager_req_launch {
    uint32_t op;           /* OP_APP_LAUNCH */
    uint32_t manifest_len; /* byte length of manifest in shmem */
};

/* OP_APP_KILL
 * MR0=op, MR1=app_id
 */
struct __attribute__((packed)) app_manager_req_kill {
    uint32_t op;
    uint32_t app_id;
};

/* OP_APP_STATUS
 * MR0=op, MR1=app_id
 */
struct __attribute__((packed)) app_manager_req_status {
    uint32_t op;
    uint32_t app_id;
};

/* OP_APP_LIST
 * MR0=op  (results written into app_list_shmem)
 */
struct __attribute__((packed)) app_manager_req_list {
    uint32_t op;
};

/* OP_APP_HEALTH
 * MR0=op
 */
struct __attribute__((packed)) app_manager_req_health {
    uint32_t op;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

/* OP_APP_LAUNCH reply: MR0=status, MR1=app_id, MR2=vnic_id */
struct __attribute__((packed)) app_manager_reply_launch {
    uint32_t status;  /* enum app_manager_error */
    uint32_t app_id;
    uint32_t vnic_id; /* virtual NIC handle, 0 if none */
};

/* OP_APP_KILL reply: MR0=status */
struct __attribute__((packed)) app_manager_reply_kill {
    uint32_t status;
};

/* OP_APP_STATUS reply: MR0=status, MR1=state, MR2=vnic_id, MR3=spawn_slot */
struct __attribute__((packed)) app_manager_reply_status {
    uint32_t status;
    uint32_t state;       /* 0=NONE 1=LAUNCHING 2=RUNNING 3=DYING */
    uint32_t vnic_id;
    uint32_t spawn_slot;
};

/* OP_APP_LIST reply: MR0=status, MR1=count */
struct __attribute__((packed)) app_manager_reply_list {
    uint32_t status;
    uint32_t count;   /* number of app_info_t entries written to shmem */
};

/* OP_APP_HEALTH reply: MR0=ok, MR1=running_count, MR2=APP_MANAGER_VERSION */
struct __attribute__((packed)) app_manager_reply_health {
    uint32_t ok;
    uint32_t running_count;
    uint32_t app_manager_version;
};

/* ─── Shmem layout: application info entry (OP_APP_LIST) ────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t app_id;
    uint32_t state;
    uint32_t vnic_id;
    uint32_t spawn_slot;
    uint8_t  name[48];   /* NUL-terminated application name */
} app_info_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum app_manager_error {
    APP_OK          = 0,
    APP_ERR_INVAL   = 1,
    APP_ERR_NO_SLOTS= 2,
    APP_ERR_SPAWN   = 3,
    APP_ERR_NOT_FOUND=4,
};
