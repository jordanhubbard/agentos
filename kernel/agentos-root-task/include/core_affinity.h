/*
 * agentOS Core Affinity PD — public header
 *
 * Passive PD (priority 200) that pins long-running GPU inference agents to
 * dedicated seL4 cores on sparky GB10 (28-core ARM), preventing scheduling
 * interference between GPU agents and background PDs.
 *
 * GB10 core topology assumed:
 *   [0,  3]  — GPU-adjacent performance cores (Cortex-A78 cluster)
 *   [4, 19]  — General compute cores (Cortex-A78 cluster)
 *   [20, 27] — ARM efficiency cores (Cortex-A55 cluster)
 *
 * Opcodes (MR0):
 *   OP_AFFINITY_PIN      (0xB0): MR1=slot_id, MR2=core_id, MR3=flags
 *   OP_AFFINITY_UNPIN    (0xB1): MR1=slot_id
 *   OP_AFFINITY_STATUS   (0xB2): → MR0=ok, MR1=slot_count, MR2=migrations,
 *                                   then 3 MRs per slot: slot_id, core_id, pinned_at_tick
 *   OP_AFFINITY_RESERVE  (0xB3): MR1=core_id (reserve for exclusive use)
 *   OP_AFFINITY_UNRESERVE(0xB4): MR1=core_id
 *   OP_AFFINITY_SUGGEST  (0xB5): MR1=slot_priority, MR2=flags → MR0=suggested_core_id
 *   OP_AFFINITY_RESET    (0xB6): clear all assignments and reservations
 *
 * Return codes (MR0 on error paths):
 *   CA_OK          0x00  success
 *   CA_ERR_FULL    0x01  slot table full (MAX_SLOTS reached)
 *   CA_ERR_NOENT   0x02  slot_id not found in assignment table
 *   CA_ERR_INVAL   0x03  invalid core_id (>= MAX_CORES)
 *   CA_ERR_BUSY    0x04  core already reserved by another slot
 *   CA_ERR_NOCORE  0x05  no suitable core found (OP_AFFINITY_SUGGEST)
 */

#pragma once

#include <stdint.h>

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

#define OP_AFFINITY_PIN       0xB0u
#define OP_AFFINITY_UNPIN     0xB1u
#define OP_AFFINITY_STATUS    0xB2u
#define OP_AFFINITY_RESERVE   0xB3u
#define OP_AFFINITY_UNRESERVE 0xB4u
#define OP_AFFINITY_SUGGEST   0xB5u
#define OP_AFFINITY_RESET     0xB6u

/* ── Return codes ────────────────────────────────────────────────────────── */

#define CA_OK          0x00u
#define CA_ERR_FULL    0x01u
#define CA_ERR_NOENT   0x02u
#define CA_ERR_INVAL   0x03u
#define CA_ERR_BUSY    0x04u
#define CA_ERR_NOCORE  0x05u

/* ── Flags (MR3 for OP_AFFINITY_PIN, MR2 for OP_AFFINITY_SUGGEST) ────────── */

#define CORE_FLAG_GPU       (1u << 0)  /* slot runs GPU inference workload */
#define CORE_FLAG_BG        (1u << 1)  /* slot is a background task */
#define CORE_FLAG_EXCLUSIVE (1u << 2)  /* no other slots may share this core */

/* ── Core range constants (GB10 topology) ────────────────────────────────── */

#define GB10_GPU_CORE_LO    0u   /* GPU-adjacent performance cores: [0, 3] */
#define GB10_GPU_CORE_HI    3u
#define GB10_BG_CORE_LO    20u  /* ARM efficiency cores: [20, 27] */
#define GB10_BG_CORE_HI    27u

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define MAX_CORES  32u
#define MAX_SLOTS  16u

/* ── Data structures ─────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  slot_id;
    uint8_t  core_id;
    uint8_t  exclusive;      /* 1 = no other slots on this core */
    uint8_t  _pad;
    uint32_t pinned_at_tick;
    uint32_t flags;          /* CORE_FLAG_GPU | CORE_FLAG_BG */
} CoreAssignment;

typedef struct {
    CoreAssignment slots[MAX_SLOTS];
    uint8_t        reserved_cores[MAX_CORES]; /* 1 = reserved for exclusive use */
    uint8_t        slot_count;
    uint8_t        _pad[3];
    uint32_t       migrations;  /* times a slot was moved to a different core */
} CoreAffinityState;

/* ── Channel IDs (from core_affinity PD perspective) ─────────────────────── */

#define CA_CH_CONTROLLER     0u  /* controller PPCs into core_affinity */
#define CA_CH_TIME_PARTITION 1u  /* core_affinity PPCs into time_partition */
