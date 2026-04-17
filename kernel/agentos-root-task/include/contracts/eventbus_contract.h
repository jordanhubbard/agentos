/*
 * EventBus IPC Contract
 *
 * The EventBus is the publish-subscribe backbone of agentOS.
 * All PDs that need to exchange asynchronous events go through EventBus.
 *
 * Channel: EVENTBUS_CH_* (see agentos.h)
 * Opcodes: MSG_EVENTBUS_* (see agentos.h)
 *
 * Invariants:
 *   - Subscribers read the ring directly (zero-copy); EventBus only writes.
 *   - PUBLISH_BATCH is atomic: all events in a batch are written before any
 *     subscriber is notified.
 *   - A subscriber that falls behind loses events (overflow_count incremented).
 *   - MSG_EVENTBUS_QUERY_SUBSCRIBERS is read-only; it does not modify state.
 *   - Ordering: INIT must be the first message sent to EventBus at boot.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs (EventBus perspective) ─────────────────────────────────── */
#define EVENTBUS_CH_MONITOR    1  /* monitor → eventbus */
#define EVENTBUS_CH_INITAGENT  2  /* init_agent → eventbus */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct eventbus_req_init {
    uint32_t version;           /* caller's agentos version */
};

struct eventbus_req_subscribe {
    uint32_t notify_ch;         /* channel to signal on new events */
    uint32_t topic_mask;        /* event kind bitmask; 0 = all events */
};

struct eventbus_req_unsubscribe {
    uint32_t notify_ch;         /* channel to remove */
};

struct eventbus_req_publish_batch {
    uint32_t count;             /* number of batch_event_t entries */
    uint32_t offset;            /* byte offset into shared eventbus ring region */
};

struct eventbus_req_status {
    /* no fields — query-only */
};

struct eventbus_req_query_subscribers {
    /* no fields — returns count + subscriber list in shmem */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct eventbus_reply_init {
    uint32_t ok;                /* 0 = success */
    uint32_t capacity;          /* ring capacity in event slots */
};

struct eventbus_reply_subscribe {
    uint32_t ok;                /* 0 = success */
    uint32_t subscriber_id;     /* assigned subscriber index */
};

struct eventbus_reply_unsubscribe {
    uint32_t ok;
};

struct eventbus_reply_publish_batch {
    uint32_t dispatched;        /* events written to ring */
    uint32_t dropped;           /* events dropped (ring full) */
};

struct eventbus_reply_status {
    uint32_t head;              /* ring write index */
    uint32_t tail;              /* ring read index (min across subscribers) */
    uint32_t overflow_count;    /* total events dropped since boot */
    uint32_t subscriber_count;  /* active subscriber count */
};

struct eventbus_reply_query_subscribers {
    uint32_t count;             /* subscriber entries written to shmem */
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum eventbus_error {
    EVENTBUS_OK               = 0,
    EVENTBUS_ERR_NOT_INIT     = 1,  /* INIT not yet received */
    EVENTBUS_ERR_FULL         = 2,  /* subscriber table full (MAX_SUBSCRIBERS) */
    EVENTBUS_ERR_NOT_FOUND    = 3,  /* unsubscribe: channel not registered */
    EVENTBUS_ERR_BAD_OFFSET   = 4,  /* batch offset outside staging region */
    EVENTBUS_ERR_BAD_COUNT    = 5,  /* batch count > PUBLISH_BATCH_MAX */
};
