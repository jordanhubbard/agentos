#pragma once
/* WATCHDOG contract — version 1
 * PD: watchdog_pd | Source: src/watchdog_pd.c
 * Channel: CH_WATCHDOG_CTRL=56 (controller PPCs in), CH_WATCHDOG_NOTIFY=54 (watchdog notifies controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define WATCHDOG_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_WATCHDOG_CTRL           56   /* controller -> watchdog_pd (PPC); cross-ref: agentos.h */
#define CH_WATCHDOG_NOTIFY         54   /* watchdog_pd -> controller (notify on timeout); cross-ref: agentos.h */

/* ── Opcodes ── */
#define WATCHDOG_OP_REGISTER       0x50u  /* MR1=slot_id, MR2=heartbeat_ticks: start monitoring */
#define WATCHDOG_OP_HEARTBEAT      0x51u  /* MR1=slot_id: update heartbeat tick */
#define WATCHDOG_OP_STATUS         0x52u  /* MR1=slot_id -> MR0=status, MR1=ticks_remaining */
#define WATCHDOG_OP_UNREGISTER     0x53u  /* MR1=slot_id: stop monitoring slot */
#define WATCHDOG_OP_FREEZE         0x54u  /* MR1=slot_id: suspend monitoring (hotreload in progress) */
#define WATCHDOG_OP_RESUME         0x55u  /* MR1=slot_id, MR2=new_module_hash_lo: resume + update hash */

/* ── Watchdog status return codes (cross-ref: agentos.h WD_*) ── */
#define WD_OK                       0x00u  /* operation successful */
#define WD_ERR_NOENT                0x01u  /* slot_id not registered */
#define WD_ERR_FULL                 0x02u  /* watchdog slot table full */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* WATCHDOG_OP_REGISTER */
    uint32_t slot_id;         /* worker/swap slot to monitor */
    uint32_t heartbeat_ticks; /* ticks between required heartbeats */
    uint64_t module_hash_lo;  /* current module hash for this slot */
    uint64_t module_hash_hi;
} watchdog_req_register_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* WD_OK or WD_ERR_FULL */
} watchdog_reply_register_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* WATCHDOG_OP_HEARTBEAT */
    uint32_t slot_id;
} watchdog_req_heartbeat_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* WD_OK or WD_ERR_NOENT */
    uint32_t ticks_remaining; /* ticks until next timeout (0 = timed out) */
} watchdog_reply_heartbeat_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* WATCHDOG_OP_STATUS */
    uint32_t slot_id;
} watchdog_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* WD_OK or WD_ERR_NOENT */
    uint32_t ticks_remaining;
    uint32_t frozen;          /* 1 if slot is in frozen (hotreload) state */
    uint32_t timeout_count;   /* number of timeouts recorded for this slot */
    uint64_t module_hash_lo;
    uint64_t module_hash_hi;
} watchdog_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* WATCHDOG_OP_UNREGISTER */
    uint32_t slot_id;
} watchdog_req_unregister_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* WD_OK or WD_ERR_NOENT */
} watchdog_reply_unregister_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* WATCHDOG_OP_FREEZE */
    uint32_t slot_id;
} watchdog_req_freeze_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* WD_OK or WD_ERR_NOENT */
} watchdog_reply_freeze_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* WATCHDOG_OP_RESUME */
    uint32_t slot_id;
    uint64_t new_module_hash_lo;  /* updated module hash after hotreload */
    uint64_t new_module_hash_hi;
    uint32_t new_heartbeat_ticks; /* updated heartbeat interval (0 = keep existing) */
} watchdog_req_resume_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* WD_OK or WD_ERR_NOENT */
} watchdog_reply_resume_t;

/* Notification sent by watchdog_pd to controller on CH_WATCHDOG_NOTIFY */
typedef struct __attribute__((packed)) {
    uint32_t slot_id;         /* slot that timed out */
    uint32_t timeout_count;   /* how many times this slot has timed out */
    uint64_t module_hash_lo;  /* module hash at time of timeout */
    uint64_t module_hash_hi;
} watchdog_notify_timeout_t;

/* ── Error codes ── */
typedef enum {
    WATCHDOG_OK               = 0,  /* maps to WD_OK */
    WATCHDOG_ERR_NOT_FOUND    = 1,  /* maps to WD_ERR_NOENT */
    WATCHDOG_ERR_FULL         = 2,  /* maps to WD_ERR_FULL */
    WATCHDOG_ERR_ALREADY_FROZEN = 3, /* freeze called on already-frozen slot */
    WATCHDOG_ERR_NOT_FROZEN   = 4,  /* resume called on non-frozen slot */
} watchdog_error_t;

/* ── Invariants ──
 * - REGISTER must be called before HEARTBEAT, STATUS, FREEZE, RESUME, or UNREGISTER.
 * - FREEZE prevents timeout notifications; use during hotreload to avoid false positives.
 * - RESUME must provide the new module hash; heartbeat clock resets at resume time.
 * - Timeout notification is sent via seL4 notify on CH_WATCHDOG_NOTIFY (not PPC).
 * - Controller must call UNREGISTER when a slot is destroyed to free watchdog resources.
 */
