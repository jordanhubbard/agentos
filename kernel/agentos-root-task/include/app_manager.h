/*
 * agentOS AppManager — Public Interface
 *
 * AppManager is an active PD (priority 130) that orchestrates full-stack
 * application deployment.  It reads TOML-style manifests from app_manifest_shmem,
 * creates vNICs via NetServer, launches ELF binaries via SpawnServer, and
 * registers HTTP routes with the http_svc PD.
 *
 * Manifest format (key=value, one per line):
 *   name=<app name, max 32 chars>
 *   elf=<VFS path to ELF binary, max 128 chars>
 *   caps=<integer bitmask of CAP_CLASS_* values>
 *   http_prefix=<URL prefix for routing, e.g. /api/myapp, max 64 chars>
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── AppManager version ──────────────────────────────────────────────────── */
#define APP_MANAGER_VERSION     1u

/* ── Console slot / pd_id ─────────────────────────────────────────────────── */
#define APP_MANAGER_CONSOLE_SLOT  18u
#define APP_MANAGER_PD_ID         18u

/* ── Channel IDs (from AppManager's perspective) ─────────────────────────── */
#define APP_CH_CONTROLLER   0   /* controller PPCs in (pp=true) */
#define APP_CH_SPAWN        1   /* AppManager PPCs OUT → SpawnServer */
#define APP_CH_VFS          2   /* AppManager PPCs OUT → VFS Server */
#define APP_CH_NET          3   /* AppManager PPCs OUT → NetServer */
#define APP_CH_HTTP         4   /* AppManager PPCs OUT → http_svc */

/* ── IPC Opcodes (MR0 in PPC calls to AppManager) ───────────────────────── */

/*
 * OP_APP_LAUNCH (0xC0) — deploy a full-stack app from a manifest
 *
 * Caller writes manifest text into app_manifest_shmem before the PPC.
 * AppManager parses it, provisions resources (vNIC, spawn slot, HTTP route),
 * and returns the assigned app_id.
 *
 *   MR1 = manifest_len  (byte count of manifest text at app_manifest_shmem[0])
 *   MR2 = flags         (reserved, must be 0)
 *   Reply:
 *   MR0 = result (APP_OK or APP_ERR_*)
 *   MR1 = app_id   (assigned application identifier)
 *   MR2 = vnic_id  (assigned vNIC, 0xFFFFFFFF if NetServer unavailable)
 */
#define OP_APP_LAUNCH       0xC0u

/*
 * OP_APP_KILL (0xC1) — stop a running application and free its resources
 *   MR1 = app_id
 *   Reply:
 *   MR0 = result
 */
#define OP_APP_KILL         0xC1u

/*
 * OP_APP_STATUS (0xC2) — query the lifecycle state of a specific app
 *   MR1 = app_id
 *   Reply:
 *   MR0 = result
 *   MR1 = state           (APP_STATE_*)
 *   MR2 = vnic_id
 *   MR3 = spawn_slot_index
 */
#define OP_APP_STATUS       0xC2u

/*
 * OP_APP_LIST (0xC3) — enumerate all apps
 *   Writes an array of app_list_entry_t into app_manifest_shmem starting at
 *   APP_MANIFEST_LIST_OFF.  Caller must not write a manifest concurrently.
 *   Reply:
 *   MR0 = result
 *   MR1 = entry_count
 */
#define OP_APP_LIST         0xC3u

/*
 * OP_APP_HEALTH (0xC4) — liveness check
 *   Reply:
 *   MR0 = APP_OK
 *   MR1 = running_app_count
 *   MR2 = APP_MANAGER_VERSION
 */
#define OP_APP_HEALTH       0xC4u

/* ── Return codes (MR0 in replies) ──────────────────────────────────────── */
#define APP_OK              0u
#define APP_ERR_NO_SLOTS    1u  /* all spawn slots are occupied */
#define APP_ERR_NOT_FOUND   2u  /* app_id not registered */
#define APP_ERR_VFS         3u  /* VFS error reading manifest or ELF */
#define APP_ERR_NET         4u  /* NetServer error creating vNIC */
#define APP_ERR_SPAWN       5u  /* SpawnServer error launching PD */
#define APP_ERR_INVAL       6u  /* invalid/malformed manifest or argument */
#define APP_ERR_HTTP        7u  /* http_svc route registration failed */

/* ── Application lifecycle states ───────────────────────────────────────── */
#define APP_STATE_FREE      0u  /* slot available */
#define APP_STATE_LOADING   1u  /* resources being provisioned */
#define APP_STATE_RUNNING   2u  /* app is live */
#define APP_STATE_STOPPING  3u  /* teardown in progress */

/* ── Manifest shmem layout (app_manifest_shmem, 16KB = 0x4000) ──────────── */
#define APP_MANIFEST_SHMEM_SIZE   0x4000u   /* 16KB total */
#define APP_MANIFEST_TEXT_MAX     0x1000u   /* 4KB: manifest key=value text */
#define APP_MANIFEST_LIST_OFF     0x1000u   /* app_list_entry_t array starts here */

/* ── Manifest field size limits ──────────────────────────────────────────── */
#define APP_MANIFEST_NAME_MAX     32u
#define APP_MANIFEST_ELF_MAX      128u
#define APP_MANIFEST_PREFIX_MAX   64u

/* ── Maximum concurrent applications ────────────────────────────────────── */
#define APP_MAX_APPS              4u   /* matches SPAWN_MAX_SLOTS */

/* ── Default capability bundle for launched apps ─────────────────────────── */
/* FS + NET + IPC + STDIO; no GPU/SPAWN/SWAP without explicit manifest override */
#define APP_DEFAULT_CAPS  ((1 << 0) | (1 << 1) | (1 << 3) | (1 << 5))
/* CAP_CLASS_FS=1, CAP_CLASS_NET=2, CAP_CLASS_IPC=8, CAP_CLASS_STDIO=32 */

/* ── List entry written by OP_APP_LIST ───────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t app_id;
    uint32_t state;                            /* APP_STATE_* */
    uint32_t vnic_id;                          /* 0xFFFFFFFF if none */
    uint32_t spawn_slot;                       /* SpawnServer slot index */
    uint32_t cap_classes;                      /* CAP_CLASS_* bitmask */
    uint32_t flags;                            /* reserved */
    char     name[APP_MANIFEST_NAME_MAX];
    char     http_prefix[APP_MANIFEST_PREFIX_MAX];
    uint64_t launch_tick;
} app_list_entry_t;                            /* 120 bytes */

/* Maximum list entries that fit after APP_MANIFEST_LIST_OFF */
#define APP_LIST_MAX_ENTRIES \
    ((APP_MANIFEST_SHMEM_SIZE - APP_MANIFEST_LIST_OFF) / sizeof(app_list_entry_t))

/* ── Parsed manifest structure (internal to AppManager) ─────────────────── */
typedef struct {
    char     name[APP_MANIFEST_NAME_MAX];
    char     elf_path[APP_MANIFEST_ELF_MAX];
    char     http_prefix[APP_MANIFEST_PREFIX_MAX];
    uint32_t cap_classes;
} app_manifest_t;
