/*
 * agentOS SpawnServer — Public Interface
 *
 * SpawnServer dynamically launches application protection domains by
 * assigning pre-allocated "app slots" (app_slot_0..3) and loading ELF
 * images into them via shared memory.  Callers issue PPCs with spawn
 * opcodes; large data (app name, ELF path, slot info) is exchanged
 * through the spawn_config_shmem region (4 KB).  The ELF binary itself
 * is staged in spawn_elf_shmem (512 KB) together with a 64-byte launch
 * header before the target slot is notified.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── SpawnServer version ─────────────────────────────────────────────────── */
#define SPAWN_VERSION       1u

/* ── Console slot / pd_id for SpawnServer ────────────────────────────────── */
#define SPAWN_CONSOLE_SLOT  15u
#define SPAWN_PD_ID         15u

/* ── Channel IDs (from SpawnServer's perspective) ───────────────────────── */
#define SPAWN_CH_CONTROLLER  0   /* controller PPCs in (pp=true) */
#define SPAWN_CH_INIT_AGENT  1   /* init_agent PPCs in (pp=true) */
#define SPAWN_CH_APP_MANAGER 2   /* app_manager PPCs in (pp=true) */
#define SPAWN_CH_VFS         3   /* SpawnServer PPCs OUT into VFS */
#define SPAWN_CH_APP_SLOT_0  4   /* SpawnServer notifies OUT → slot 0 */
#define SPAWN_CH_APP_SLOT_1  5
#define SPAWN_CH_APP_SLOT_2  6
#define SPAWN_CH_APP_SLOT_3  7

/* Number of pre-allocated app slots */
#define SPAWN_MAX_SLOTS      4

/* ── SpawnServer opcodes (MR0 in PPC calls) ─────────────────────────────── */
#define OP_SPAWN_LAUNCH      0xA0u  /* Launch a new app by ELF path */
#define OP_SPAWN_KILL        0xA1u  /* Kill a running app by app_id */
#define OP_SPAWN_STATUS      0xA2u  /* Query status of an app slot */
#define OP_SPAWN_LIST        0xA3u  /* List all slots → spawn_config_shmem */
#define OP_SPAWN_HEALTH      0xA4u  /* Health check for SpawnServer itself */

/* ── Return codes (MR0 in SpawnServer replies) ───────────────────────────── */
#define SPAWN_OK             0u
#define SPAWN_ERR_NO_SLOTS   1u  /* All app slots are occupied */
#define SPAWN_ERR_NOT_FOUND  2u  /* app_id not found */
#define SPAWN_ERR_VFS        3u  /* VFS error during ELF load */
#define SPAWN_ERR_TOO_LARGE  4u  /* ELF binary exceeds staging budget */
#define SPAWN_ERR_INVAL      5u  /* Invalid argument */

/* ── App slot states ─────────────────────────────────────────────────────── */
#define SPAWN_SLOT_FREE      0u  /* Slot is available */
#define SPAWN_SLOT_LOADING   1u  /* ELF staged, slot notified, waiting */
#define SPAWN_SLOT_RUNNING   2u  /* Slot notified back: app is live */
#define SPAWN_SLOT_KILLED    3u  /* App was killed, slot being recycled */

/* ── Shared memory sizes ─────────────────────────────────────────────────── */
#define SPAWN_ELF_SHMEM_SIZE    0x80000u   /* 512 KB — ELF staging area */
#define SPAWN_CONFIG_SHMEM_SIZE 0x1000u    /* 4 KB  — name/path/list area */

/* ── spawn_elf_shmem virtual addresses ──────────────────────────────────── */
#define SPAWN_ELF_SHMEM_SPAWN_VADDR      0x5000000UL  /* SpawnServer view */
#define SPAWN_ELF_SHMEM_CONTROLLER_VADDR 0xD000000UL  /* Controller view  */
#define SPAWN_ELF_SHMEM_VFS_VADDR        0x7000000UL  /* VFS view         */

/* ── spawn_config_shmem virtual addresses ───────────────────────────────── */
#define SPAWN_CONFIG_SHMEM_SPAWN_VADDR      0x6000000UL  /* SpawnServer view */
#define SPAWN_CONFIG_SHMEM_CONTROLLER_VADDR 0xD100000UL  /* Controller view  */

/* ── spawn_config_shmem layout offsets ──────────────────────────────────── */
#define SPAWN_CONFIG_NAME_OFF   0u    /* app name   [0..31]   — 32 bytes */
#define SPAWN_CONFIG_PATH_OFF   32u   /* ELF path   [32..159] — 128 bytes */
#define SPAWN_CONFIG_LIST_OFF   160u  /* spawn_slot_info_t array starts here */

#define SPAWN_CONFIG_NAME_MAX   32u
#define SPAWN_CONFIG_PATH_MAX   128u

/* ── Launch header — first 96 bytes of spawn_elf_shmem ──────────────────── */
#define SPAWN_MAGIC  0x5350574Eu  /* "SPWN" little-endian */
#define SPAWN_HEADER_SIZE  96u    /* ELF image follows at byte 96 */

/* Maximum ELF size that fits in the staging window after the header */
#define SPAWN_MAX_ELF_SIZE  (SPAWN_ELF_SHMEM_SIZE - SPAWN_HEADER_SIZE)

typedef struct __attribute__((packed)) {
    uint32_t magic;           /* SPAWN_MAGIC = 0x5350574E */
    uint32_t elf_size;        /* bytes of ELF image following this header */
    uint32_t cap_classes;     /* capability bundle granted to this app */
    uint32_t app_id;          /* assigned app ID */
    uint32_t vfs_handle;      /* handle to app's VFS namespace (future) */
    uint32_t net_vnic_id;     /* assigned vNIC (future) */
    uint8_t  name[32];        /* app name (null-terminated) */
    uint8_t  elf_sha256[32];  /* SHA-256 of ELF image bytes (written by SpawnServer,
                                *  verified by app_slot before executing) */
} spawn_header_t;             /* exactly 96 bytes */

/* ── Slot info entry written into spawn_config_shmem for OP_SPAWN_LIST ──── */
typedef struct __attribute__((packed)) {
    uint32_t app_id;
    uint32_t slot_index;
    uint32_t state;         /* SPAWN_SLOT_* */
    uint32_t cap_classes;
    char     name[32];
    uint64_t launch_tick;
} spawn_slot_info_t;        /* 48 bytes */

/* Maximum entries that fit in the list area of spawn_config_shmem */
#define SPAWN_LIST_MAX_ENTRIES \
    ((SPAWN_CONFIG_SHMEM_SIZE - SPAWN_CONFIG_LIST_OFF) / sizeof(spawn_slot_info_t))
