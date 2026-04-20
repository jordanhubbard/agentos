/*
 * AppSlot Notification Contract
 *
 * The AppSlot PD is a passive, notification-driven protection domain.
 * There are four pre-allocated instances (app_slot_0 .. app_slot_3),
 * all running the same binary.  Each slot waits for a notification from
 * SpawnServer, verifies the staged ELF, and notifies back with the result.
 *
 * AppSlot has NO protected() IPC dispatcher.  It never acts as an IPC
 * server; all communication is seL4 notification-only.
 *
 * Channel layout (from app_slot's perspective):
 *   id=0  CH_SPAWN — bidirectional notification with SpawnServer
 *           SpawnServer → app_slot : ELF has been staged in spawn_elf_shmem
 *           app_slot → SpawnServer : verification result (MR0 = ok/error)
 *
 * Shmem: spawn_elf_shmem (mapped read-only into app_slot)
 *   Layout defined by spawn_header_t (see spawn.h):
 *     [0]   spawn_header_t   — metadata written by SpawnServer before notify
 *     [N]   uint8_t elf[]    — raw ELF image bytes (size in header)
 *
 * Verification protocol:
 *   1. SpawnServer writes spawn_header_t at offset 0 of spawn_elf_shmem.
 *      The header contains the expected SHA-256 digest and the ELF byte count.
 *   2. SpawnServer calls microkit_notify(CH_SPAWN) to wake app_slot.
 *   3. app_slot reads the header, computes SHA-256 over elf[0..size-1],
 *      and compares against header.sha256.
 *   4. app_slot places the result in MR0 (APP_SLOT_OK or error code) and
 *      calls microkit_notify(CH_SPAWN) to signal SpawnServer.
 *   5. SpawnServer reads MR0 from the notification message registers.
 *
 * Invariants:
 *   - app_slot never modifies spawn_elf_shmem.
 *   - app_slot must complete verification before the next notify arrives;
 *     SpawnServer serialises requests per slot.
 *   - If SHA-256 verification fails, app_slot replies APP_SLOT_ERR_HASH
 *     and SpawnServer aborts the launch.
 *   - app_slot has no persistent state; each notification is independent.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs (from app_slot perspective) ───────────────────────────── */
#define APP_SLOT_CH_SPAWN   0    /* notification channel to/from SpawnServer */

/* ─── Notification result codes (placed in MR0 after verification) ───────── */
#define APP_SLOT_OK              0u   /* ELF hash verified; slot acknowledged */
#define APP_SLOT_ERR_HASH        1u   /* SHA-256 mismatch; launch aborted */
#define APP_SLOT_ERR_TRUNCATED   2u   /* ELF shorter than header.size claims */
#define APP_SLOT_ERR_INVAL       3u   /* header magic wrong or size = 0 */

/* ─── Shmem layout: spawn header (mirrors spawn.h spawn_header_t) ────────── */

#define SPAWN_HEADER_MAGIC   0x5350574eu   /* "SPWN" */

typedef struct __attribute__((packed)) {
    uint32_t magic;              /* SPAWN_HEADER_MAGIC */
    uint32_t app_id;             /* app slot index (0..3) */
    uint32_t elf_size;           /* byte length of the ELF image */
    uint8_t  sha256[32];         /* expected SHA-256 digest of elf[0..size-1] */
    uint8_t  name[64];           /* NUL-terminated application name */
} app_slot_spawn_header_t;
