/*
 * Watchdog IPC Contract
 *
 * The Watchdog PD monitors swap slot health via periodic heartbeat ticks.
 * If a slot misses its heartbeat deadline, the Watchdog notifies the
 * controller to take corrective action.
 *
 * Channel: CH_WATCHDOG_CTRL (PPCs from controller), CH_WATCHDOG_NOTIFY (notifications)
 * Opcodes: OP_WD_REGISTER, OP_WD_HEARTBEAT, OP_WD_STATUS, OP_WD_UNREGISTER,
 *          OP_WD_FREEZE, OP_WD_RESUME
 *
 * Invariants:
 *   - REGISTER must precede HEARTBEAT and STATUS for a given slot_id.
 *   - FREEZE suspends watchdog monitoring during hot-reload; RESUME re-enables it.
 *   - If a slot misses heartbeat_ticks consecutive ticks, a notification is sent
 *     on CH_WATCHDOG_NOTIFY.
 *   - UNREGISTER removes the slot from monitoring permanently.
 *   - OP_WD_STATUS is read-only.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define WATCHDOG_CH_CTRL    CH_WATCHDOG_CTRL
#define WATCHDOG_CH_NOTIFY  CH_WATCHDOG_NOTIFY

/* ─── Request structs (MR0 = opcode, MR1..MRn = args) ──────────────────── */

struct wd_req_register {
    uint32_t op;                /* OP_WD_REGISTER */
    uint32_t slot_id;
    uint32_t heartbeat_ticks;   /* miss window before notification */
};

struct wd_req_heartbeat {
    uint32_t op;                /* OP_WD_HEARTBEAT */
    uint32_t slot_id;
};

struct wd_req_status {
    uint32_t op;                /* OP_WD_STATUS */
    uint32_t slot_id;
};

struct wd_req_unregister {
    uint32_t op;                /* OP_WD_UNREGISTER */
    uint32_t slot_id;
};

struct wd_req_freeze {
    uint32_t op;                /* OP_WD_FREEZE */
    uint32_t slot_id;
};

struct wd_req_resume {
    uint32_t op;                /* OP_WD_RESUME */
    uint32_t slot_id;
    uint64_t new_module_hash_lo;  /* updated module hash after hot-reload */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct wd_reply_register {
    uint32_t result;            /* WD_OK or WD_ERR_* */
};

struct wd_reply_heartbeat {
    uint32_t result;
};

struct wd_reply_status {
    uint32_t result;
    uint32_t state;             /* WD_STATE_* */
    uint32_t ticks_remaining;
};

#define WD_STATE_MONITORING  0
#define WD_STATE_FROZEN      1
#define WD_STATE_EXPIRED     2

struct wd_reply_unregister {
    uint32_t result;
};

struct wd_reply_freeze {
    uint32_t result;
};

struct wd_reply_resume {
    uint32_t result;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */
/* WD_OK, WD_ERR_NOENT, WD_ERR_FULL defined in agentos.h */
