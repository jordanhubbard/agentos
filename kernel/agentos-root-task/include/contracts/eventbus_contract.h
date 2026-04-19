#pragma once
/* EVENTBUS contract — version 1
 * PD: event_bus | Source: src/event_bus.c | Channel: EVENTBUS_CH_MONITOR=1, EVENTBUS_CH_INITAGENT=2 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define EVENTBUS_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_EVENTBUS            0   /* controller -> event_bus (from controller enum: CH_CONTROLLER_EVENT_BUS) */
#define EVENTBUS_CH_MONITOR    1   /* cross-ref: agentos.h */
#define EVENTBUS_CH_INITAGENT  2   /* cross-ref: agentos.h */

/* ── Opcodes ── */
#define EVENTBUS_OP_INIT            0x0001u  /* initialize event bus ring buffer */
#define EVENTBUS_OP_SUBSCRIBE       0x0002u  /* subscribe a channel to a topic mask */
#define EVENTBUS_OP_UNSUBSCRIBE     0x0003u  /* remove subscription */
#define EVENTBUS_OP_STATUS          0x0004u  /* query ring buffer statistics */
#define EVENTBUS_OP_PUBLISH_BATCH   0x0005u  /* publish up to 16 events in one PPC */
#define EVENTBUS_OP_READY           0x0101u  /* event_bus -> controller: init complete */
#define EVENTBUS_OP_ERROR           0x0102u  /* event_bus -> controller: fatal error */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* EVENTBUS_OP_INIT */
    uint32_t ring_capacity;   /* number of event slots to initialize */
} eventbus_req_init_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t ring_capacity;   /* actual capacity initialized */
} eventbus_reply_init_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* EVENTBUS_OP_SUBSCRIBE */
    uint32_t channel_id;      /* seL4 channel to notify on matching event */
    uint32_t topic_mask;      /* bitmask of event kinds to receive */
} eventbus_req_subscribe_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else eventbus_error_t */
    uint32_t sub_id;          /* subscription handle */
} eventbus_reply_subscribe_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* EVENTBUS_OP_UNSUBSCRIBE */
    uint32_t sub_id;          /* subscription handle from subscribe reply */
} eventbus_req_unsubscribe_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} eventbus_reply_unsubscribe_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* EVENTBUS_OP_STATUS */
} eventbus_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint64_t head;            /* current write index */
    uint64_t tail;            /* current read index */
    uint32_t overflow_count;  /* events dropped due to full ring */
    uint32_t subscriber_count;
} eventbus_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* EVENTBUS_OP_PUBLISH_BATCH */
    uint32_t event_count;     /* number of events (1-16) */
    uint32_t shmem_offset;    /* byte offset into shared ring region for batch entries */
} eventbus_req_publish_batch_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t dispatched;      /* events written to ring */
    uint32_t dropped;         /* events dropped (ring full) */
} eventbus_reply_publish_batch_t;

/* ── Error codes ── */
typedef enum {
    EVENTBUS_OK               = 0,
    EVENTBUS_ERR_OVERFLOW     = 1,  /* ring buffer full; event dropped */
    EVENTBUS_ERR_NO_SUBS      = 2,  /* no subscribers for topic */
    EVENTBUS_ERR_BAD_SUB      = 3,  /* invalid subscription handle */
    EVENTBUS_ERR_FULL_SUBS    = 4,  /* subscription table full */
    EVENTBUS_ERR_NOT_INIT     = 5,  /* ring not initialized */
    EVENTBUS_ERR_BAD_COUNT    = 6,  /* batch count out of range (0 or >16) */
} eventbus_error_t;

/* ── Invariants ──
 * - Ring capacity must be a power of two.
 * - Overflow increments overflow_count atomically; callers must tolerate gaps.
 * - Subscribers must read faster than producers write or events will be dropped.
 * - EVENTBUS_OP_PUBLISH_BATCH requires shared ring memory mapped before call.
 * - Maximum 16 events per PUBLISH_BATCH call (PUBLISH_BATCH_MAX in agentos.h).
 */
