#pragma once
/* OOM_KILLER contract — version 1
 * PD: oom_killer | Source: src/oom_killer.c | Channel: (triggered by kernel OOM notification)
 */
#include <stdint.h>
#include <stdbool.h>

#define OOM_KILLER_CONTRACT_VERSION 1

/* ── Opcodes ── */
#define OOM_KILLER_OP_STATUS       0xF100u  /* query OOM killer state and victim statistics */
#define OOM_KILLER_OP_POLICY_SET   0xF101u  /* set OOM selection and recovery policy */
#define OOM_KILLER_OP_NOTIFY       0xF102u  /* OOM event notification (kernel -> oom_killer) */
#define OOM_KILLER_OP_EXEMPT       0xF103u  /* mark a slot as exempt from OOM killing */
#define OOM_KILLER_OP_SCORE        0xF104u  /* query OOM score for a specific slot */

/* ── OOM selection policy ── */
#define OOM_POLICY_LOWEST_PRIORITY  0u  /* kill lowest-priority slot first */
#define OOM_POLICY_LARGEST_MEM      1u  /* kill largest memory consumer first */
#define OOM_POLICY_OLDEST_IDLE      2u  /* kill oldest idle slot first */
#define OOM_POLICY_MANUAL           3u  /* controller must select victim explicitly */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* OOM_KILLER_OP_STATUS */
} oom_killer_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t oom_events;      /* total OOM events handled since boot */
    uint32_t slots_killed;    /* total slots terminated by OOM killer */
    uint32_t free_pages;      /* current free physical pages */
    uint32_t total_pages;     /* total physical pages in system */
    uint32_t policy;          /* OOM_POLICY_* currently active */
} oom_killer_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* OOM_KILLER_OP_POLICY_SET */
    uint32_t policy;          /* OOM_POLICY_* */
    uint32_t low_watermark_pages;   /* pages below which OOM is triggered */
    uint32_t high_watermark_pages;  /* pages above which system is considered healthy */
    uint32_t min_exempt_pages;      /* pages to leave free even when killing */
} oom_killer_req_policy_set_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else oom_killer_error_t */
} oom_killer_reply_policy_set_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* OOM_KILLER_OP_NOTIFY */
    uint32_t free_pages;      /* pages available at time of OOM notification */
    uint32_t requesting_slot; /* slot that triggered OOM (0 = system-wide) */
    uint32_t requested_pages; /* pages the requesting slot needs */
} oom_killer_req_notify_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = victim selected and kill initiated */
    uint32_t victim_slot_id;  /* slot selected for termination */
    uint32_t freed_estimate;  /* estimated pages to be freed */
} oom_killer_reply_notify_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* OOM_KILLER_OP_EXEMPT */
    uint32_t slot_id;
    uint32_t exempt;          /* 1 = add exemption, 0 = remove exemption */
} oom_killer_req_exempt_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} oom_killer_reply_exempt_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* OOM_KILLER_OP_SCORE */
    uint32_t slot_id;
} oom_killer_req_score_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t oom_score;       /* computed OOM score (higher = more likely to be killed) */
    uint32_t pages_used;      /* physical pages used by this slot */
    uint32_t priority;        /* slot scheduling priority */
    uint32_t exempt;          /* 1 if slot is OOM-exempt */
} oom_killer_reply_score_t;

/* ── Error codes ── */
typedef enum {
    OOM_KILLER_OK             = 0,
    OOM_KILLER_ERR_NO_VICTIM  = 1,  /* no killable slot found (all exempt or empty) */
    OOM_KILLER_ERR_BAD_SLOT   = 2,  /* slot_id not valid */
    OOM_KILLER_ERR_BAD_POLICY = 3,  /* invalid policy or watermark configuration */
    OOM_KILLER_ERR_NO_CAP     = 4,  /* caller lacks privilege */
} oom_killer_error_t;

/* ── Invariants ──
 * - System-critical PDs (controller, event_bus, fault_handler) are permanently exempt.
 * - OOM_POLICY_MANUAL requires the controller to provide a victim_slot_id in the reply.
 * - low_watermark_pages must be less than high_watermark_pages.
 * - min_exempt_pages guarantees the OOM killer never exhausts all free pages.
 * - After killing a victim, oom_killer notifies the controller via EventBus.
 * - OOM scoring factors: memory use, priority, idle time, and explicit oom_score_adj.
 */
