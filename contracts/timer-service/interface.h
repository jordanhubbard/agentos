/*
 * contracts/timer-service/interface.h — Timer/Clock Generic Device Interface
 *
 * // STATUS: IMPLEMENTED
 *
 * This is the canonical contract for the timer-service device service in agentOS.
 * The concrete implementation is provided through seL4 MCS scheduling contexts,
 * managed by kernel/agentos-root-task/src/time_partition.c (policy enforcement)
 * and the controller's tick mechanism (periodic notification on CH_TIMER).
 *
 * The timer service provides wall-clock reads, one-shot alarms, cancellation,
 * and cooperative sleep primitives.  Every guest OS and VMM MUST use this
 * service for time-based operations rather than programming hardware timers
 * (e.g., ARM Generic Timer, x86 APIC timer) directly.
 *
 * IPC transport:
 *   - Protected procedure call (PPC) via seL4 Microkit.
 *   - MR0 = opcode (TIMER_SVC_OP_*)
 *   - MR1..MR6 = arguments (opcode-specific, see per-op comments below)
 *   - Reply: MR0 = status (TIMER_SVC_ERR_*), MR1..MR6 = result fields
 *
 * Time representation:
 *   All time values are in nanoseconds since an arbitrary monotonic epoch
 *   (system boot).  A 64-bit nanosecond counter wraps after ~584 years.
 *   Values are passed as (hi << 32 | lo) across 32-bit MR registers.
 *
 * Alarm delivery:
 *   When an alarm fires the timer service sends a notification on the
 *   channel previously registered by the caller via TIMER_SVC_OP_SET_ALARM.
 *   The alarm_id is packed into the notification word so the caller can
 *   demultiplex multiple concurrent alarms.
 *
 * Capability grant:
 *   vm_manager.c grants a PPC capability to the timer-service endpoint at
 *   guest OS creation time.  Guest OSes register an alarm callback channel
 *   via SET_ALARM and receive notifications on it.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ── Interface version ──────────────────────────────────────────────────── */
#define TIMER_SVC_INTERFACE_VERSION     1

/* ── Limits ─────────────────────────────────────────────────────────────── */
#define TIMER_SVC_MAX_ALARMS            32u   /* concurrent alarms per client PD */
#define TIMER_SVC_ALARM_ID_INVALID      0xFFFFFFFFu

/* ── Opcodes (MR0) ──────────────────────────────────────────────────────── */

/*
 * TIMER_SVC_OP_SET_ALARM (0xD0)
 * Register a one-shot alarm at an absolute or relative time.
 *
 * When the alarm fires, the timer service sends a notification on
 * notify_channel.  The alarm_id is included in the notification word
 * so the client can identify which alarm fired.
 *
 * Request:
 *   MR1 = deadline_lo     — low 32 bits of alarm deadline (nanoseconds)
 *   MR2 = deadline_hi     — high 32 bits
 *   MR3 = flags           — TIMER_SVC_FLAG_RELATIVE or TIMER_SVC_FLAG_ABSOLUTE
 *   MR4 = notify_channel  — Microkit channel id to notify on expiry
 *   MR5 = caller_pd_id    — client's PD id (for capability validation)
 * Reply:
 *   MR0 = status
 *   MR1 = alarm_id        — opaque handle for CANCEL_ALARM (INVALID on failure)
 */
#define TIMER_SVC_OP_SET_ALARM          0xD0u

/* Flags for SET_ALARM MR3 */
#define TIMER_SVC_FLAG_ABSOLUTE         0u   /* deadline is absolute ns since boot */
#define TIMER_SVC_FLAG_RELATIVE         1u   /* deadline is relative offset from now */
#define TIMER_SVC_FLAG_REPEATING        2u   /* re-arm automatically after firing */

/*
 * TIMER_SVC_OP_CANCEL_ALARM (0xD1)
 * Cancel a pending alarm before it fires.
 *
 * If the alarm has already fired this call is a benign no-op.
 *
 * Request:
 *   MR1 = alarm_id        — handle returned by TIMER_SVC_OP_SET_ALARM
 * Reply:
 *   MR0 = status          — TIMER_SVC_ERR_NOT_FOUND if alarm already fired/cancelled
 */
#define TIMER_SVC_OP_CANCEL_ALARM       0xD1u

/*
 * TIMER_SVC_OP_GET_TIME (0xD2)
 * Read the current monotonic clock value.
 *
 * The clock is nanoseconds since system boot; it never goes backwards.
 * Callers should not assume any relationship to wall-clock time.
 *
 * Request: (none beyond MR0)
 * Reply:
 *   MR0 = status
 *   MR1 = time_lo         — low 32 bits of current time (ns)
 *   MR2 = time_hi         — high 32 bits
 */
#define TIMER_SVC_OP_GET_TIME           0xD2u

/*
 * TIMER_SVC_OP_SLEEP (0xD3)
 * Suspend the calling PD for at least the specified duration.
 *
 * The call blocks (from the caller's perspective) until the sleep duration
 * has elapsed.  Implemented as a one-shot alarm + IPC reply deferral.
 * Callers should prefer asynchronous alarms (SET_ALARM) when possible to
 * avoid tying up their IPC thread.
 *
 * Request:
 *   MR1 = duration_lo     — low 32 bits of sleep duration (nanoseconds)
 *   MR2 = duration_hi     — high 32 bits
 * Reply (after sleep completes):
 *   MR0 = status
 *   MR1 = actual_sleep_lo — actual nanoseconds slept (may be >= requested)
 *   MR2 = actual_sleep_hi
 */
#define TIMER_SVC_OP_SLEEP              0xD3u

/* ── Error / status codes (MR0 in replies) ──────────────────────────────── */
#define TIMER_SVC_ERR_OK                0u   /* success */
#define TIMER_SVC_ERR_NO_SLOTS          1u   /* alarm table full */
#define TIMER_SVC_ERR_NOT_FOUND         2u   /* alarm_id not found */
#define TIMER_SVC_ERR_INVAL             3u   /* invalid argument (zero duration, etc.) */
#define TIMER_SVC_ERR_PERM              4u   /* capability check failed */
#define TIMER_SVC_ERR_PAST              5u   /* absolute deadline already passed */

/* ── Request / reply structs ─────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;
    uint32_t time_lo;         /* deadline_lo / duration_lo */
    uint32_t time_hi;         /* deadline_hi / duration_hi */
    uint32_t flags;           /* TIMER_SVC_FLAG_* */
    uint32_t notify_channel;  /* Microkit channel for alarm notification */
    uint32_t caller_pd_id;
} timer_svc_req_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* TIMER_SVC_ERR_* */
    uint32_t alarm_id;        /* SET_ALARM: opaque alarm handle */
    uint32_t time_lo;         /* GET_TIME / SLEEP: result time low */
    uint32_t time_hi;         /* GET_TIME / SLEEP: result time high */
} timer_svc_reply_t;
