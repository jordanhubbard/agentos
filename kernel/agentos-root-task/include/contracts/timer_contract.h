#pragma once
/* TIMER_PD contract — version 1
 * PD: timer_pd | Source: src/timer_pd.c | Channel: CH_TIMER_PD (65) from controller
 * Provides OS-neutral IPC timer and RTC service. Supports periodic and one-shot timers.
 */
#include <stdint.h>
#include <stdbool.h>

#define TIMER_PD_CONTRACT_VERSION 1

/* ── Channel ─────────────────────────────────────────────────────────────── */
#define CH_TIMER_PD  65u

/* ── Opcodes (from agentos_msg_tag_t) ───────────────────────────────────── */
#define TIMER_OP_CREATE     0x1040u
#define TIMER_OP_DESTROY    0x1041u
#define TIMER_OP_START      0x1042u
#define TIMER_OP_STOP       0x1043u
#define TIMER_OP_STATUS     0x1044u
#define TIMER_OP_CONFIGURE  0x1045u
#define TIMER_OP_SET_RTC    0x1046u
#define TIMER_OP_GET_RTC    0x1047u

#define TIMER_MAX_TIMERS    8u

/* ── Timer flags (used in TIMER_OP_CREATE MR2) ──────────────────────────── */
#define TIMER_FLAG_PERIODIC  (1u << 0)  /* auto-reload after period */
#define TIMER_FLAG_ONE_SHOT  (0u << 0)  /* fire once then stop */

/* ── Request structs ─────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t op;            /* TIMER_OP_CREATE */
    uint32_t period_us;     /* timer period in microseconds */
    uint32_t flags;         /* TIMER_FLAG_* bitmask */
} timer_req_create_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* TIMER_OP_DESTROY */
    uint32_t timer_id;
} timer_req_destroy_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* TIMER_OP_START */
    uint32_t timer_id;
} timer_req_start_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* TIMER_OP_STOP */
    uint32_t timer_id;
} timer_req_stop_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* TIMER_OP_STATUS */
    uint32_t timer_id;
} timer_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* TIMER_OP_CONFIGURE */
    uint32_t timer_id;
    uint32_t period_us;     /* new period */
} timer_req_configure_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* TIMER_OP_SET_RTC */
    uint32_t unix_ts_lo;    /* UNIX timestamp low 32 bits */
    uint32_t unix_ts_hi;    /* UNIX timestamp high 32 bits */
} timer_req_set_rtc_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* TIMER_OP_GET_RTC */
} timer_req_get_rtc_t;

/* ── Reply structs ───────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t result;        /* 0 = ok */
    uint32_t timer_id;      /* assigned timer ID */
} timer_reply_create_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t running;       /* 1 = active, 0 = stopped */
    uint32_t elapsed_us;    /* microseconds since last start or fire */
} timer_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t unix_ts_lo;
    uint32_t unix_ts_hi;
} timer_reply_get_rtc_t;

/* ── Error codes ─────────────────────────────────────────────────────────── */
typedef enum {
    TIMER_OK              = 0,
    TIMER_ERR_NO_SLOT     = 1,  /* no free timer slots */
    TIMER_ERR_BAD_SLOT    = 2,  /* invalid timer_id */
    TIMER_ERR_BAD_PERIOD  = 3,  /* period_us is zero or out of range */
    TIMER_ERR_HW          = 4,  /* hardware timer error */
    TIMER_ERR_NOT_IMPL    = 5,  /* operation not yet implemented */
} timer_error_t;

/* ── Invariants ──────────────────────────────────────────────────────────
 * - TIMER_OP_START on an already-running timer resets the period without error.
 * - TIMER_OP_STOP on a stopped timer is a no-op (returns TIMER_OK).
 * - TIMER_OP_DESTROY implicitly stops the timer first.
 * - RTC value persists across STOP/START cycles but not across power cycles.
 * - Callbacks fire via microkit_notify to the channel registered at creation time (Phase 2.5).
 */
