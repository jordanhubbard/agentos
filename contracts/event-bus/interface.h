/*
 * agentOS EventBus IPC Contract — interface.h
 *
 * Formal seL4 IPC API contract for the EventBus protection domain.
 * EventBus is the pub/sub event routing layer of agentOS.  Agents publish
 * typed events to named topics; the bus delivers them to all subscribers.
 *
 * Design constraints (from the seL4 implementation):
 *   - Each topic has exactly one owner PD (first creator wins)
 *   - Only the owner may publish (prevents topic squatting)
 *   - Payloads are up to 512 bytes (MAX_PAYLOAD_BYTES)
 *   - Each subscriber has a pending queue of up to 256 events (backpressure)
 *   - Subscribers are identified by a uint32 SubscriberId
 *
 * The EventBus PD (priority 200, passive) also serves as the delivery
 * layer for MsgBus system channels.  It holds seL4 Notification capabilities
 * for every subscriber; agents never hold caps to each other directly.
 *
 * IPC mechanism: seL4_Call / seL4_Reply (passive PD).
 * MR0 carries the opcode on request; MR0 carries the status on reply.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define EVENTBUS_INTERFACE_VERSION  1

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define EVENTBUS_TOPIC_NAME_MAX     128
#define EVENTBUS_MAX_PAYLOAD_BYTES  512   /* max per-event payload */
#define EVENTBUS_MAX_PENDING        256   /* max pending events per subscriber */
#define EVENTBUS_DRAIN_BATCH_MAX    32    /* max events returned per DRAIN call */

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

#define EVENTBUS_OP_CREATE_TOPIC    0x600u  /* create and own a topic */
#define EVENTBUS_OP_SUBSCRIBE       0x601u  /* subscribe to a topic */
#define EVENTBUS_OP_UNSUBSCRIBE     0x602u  /* cancel subscription */
#define EVENTBUS_OP_PUBLISH         0x603u  /* publish event (owner only) */
#define EVENTBUS_OP_DRAIN           0x604u  /* drain pending events */
#define EVENTBUS_OP_STATUS          0x605u  /* topic/subscriber statistics */
#define EVENTBUS_OP_HEALTH          0x606u  /* liveness probe */

/* Additional opcodes from agentos.h used by the seL4 IPC label field */
#define EVENTBUS_OP_INIT            0x0001u  /* MSG_EVENTBUS_INIT */
#define EVENTBUS_OP_SUBSCRIBE_OLD   0x0002u  /* MSG_EVENTBUS_SUBSCRIBE (legacy) */
#define EVENTBUS_OP_UNSUBSCRIBE_OLD 0x0003u  /* MSG_EVENTBUS_UNSUBSCRIBE (legacy) */
#define EVENTBUS_OP_STATUS_OLD      0x0004u  /* MSG_EVENTBUS_STATUS (legacy) */
#define EVENTBUS_OP_PUBLISH_BATCH   0x0005u  /* MSG_EVENTBUS_PUBLISH_BATCH */

/* ── Error codes ─────────────────────────────────────────────────────────── */

#define EVENTBUS_ERR_OK             0u
#define EVENTBUS_ERR_INVALID_ARG    1u   /* bad opcode, null topic, etc. */
#define EVENTBUS_ERR_UNKNOWN_TOPIC  2u   /* topic not registered */
#define EVENTBUS_ERR_UNKNOWN_SUB    3u   /* subscriber ID not valid */
#define EVENTBUS_ERR_UNAUTHORIZED   4u   /* publisher is not topic owner */
#define EVENTBUS_ERR_PAYLOAD_BIG    5u   /* payload exceeds 512 bytes */
#define EVENTBUS_ERR_QUEUE_FULL     6u   /* subscriber queue at 256 limit */
#define EVENTBUS_ERR_OVERFLOW       7u   /* ring buffer full (seL4 IPC overflow) */
#define EVENTBUS_ERR_INTERNAL       99u

/* ── Packed event structure (returned in shmem by EVENTBUS_OP_DRAIN) ────── */

typedef struct eventbus_event {
    uint32_t topic_hash;                    /* CRC32 of topic name */
    uint32_t payload_len;
    uint64_t timestamp_ns;
    char     topic[EVENTBUS_TOPIC_NAME_MAX];
    uint8_t  payload[EVENTBUS_MAX_PAYLOAD_BYTES];
} __attribute__((packed)) eventbus_event_t;

/* ── Request / reply structs ─────────────────────────────────────────────── */

/*
 * EVENTBUS_OP_CREATE_TOPIC
 *
 * Create a topic and become its owner.  Only the owner PD may publish.
 * If the topic already exists the call is a no-op (first creator wins).
 *
 * owner_pd: the PD ID of the creating protection domain.
 *           Pass 0 to create a kernel-owned topic (publish allowed by all).
 *
 * Request:  opcode, owner_pd, topic
 * Reply:    status
 */
typedef struct eventbus_create_topic_req {
    uint32_t opcode;                        /* EVENTBUS_OP_CREATE_TOPIC */
    uint32_t owner_pd;
    char     topic[EVENTBUS_TOPIC_NAME_MAX];
} __attribute__((packed)) eventbus_create_topic_req_t;

typedef struct eventbus_create_topic_rep {
    uint32_t status;                        /* EVENTBUS_ERR_* */
} __attribute__((packed)) eventbus_create_topic_rep_t;

/*
 * EVENTBUS_OP_SUBSCRIBE
 *
 * Subscribe to a topic.  Returns a SubscriberId used in DRAIN and UNSUBSCRIBE.
 * The topic must exist (call CREATE_TOPIC first).
 *
 * Request:  opcode, topic
 * Reply:    status, subscriber_id
 */
typedef struct eventbus_subscribe_req {
    uint32_t opcode;                        /* EVENTBUS_OP_SUBSCRIBE */
    uint32_t _pad;
    char     topic[EVENTBUS_TOPIC_NAME_MAX];
} __attribute__((packed)) eventbus_subscribe_req_t;

typedef struct eventbus_subscribe_rep {
    uint32_t status;
    uint32_t subscriber_id;
} __attribute__((packed)) eventbus_subscribe_rep_t;

/*
 * EVENTBUS_OP_UNSUBSCRIBE
 *
 * Cancel a subscription.  Pending events for this subscriber are discarded.
 *
 * Request:  opcode, subscriber_id
 * Reply:    status
 */
typedef struct eventbus_unsubscribe_req {
    uint32_t opcode;                        /* EVENTBUS_OP_UNSUBSCRIBE */
    uint32_t subscriber_id;
} __attribute__((packed)) eventbus_unsubscribe_req_t;

typedef struct eventbus_unsubscribe_rep {
    uint32_t status;
} __attribute__((packed)) eventbus_unsubscribe_rep_t;

/*
 * EVENTBUS_OP_PUBLISH
 *
 * Publish an event to a topic.  The caller must be the topic owner.
 * payload is read from the caller's shmem MR at payload_shmem_offset.
 * payload_len must not exceed EVENTBUS_MAX_PAYLOAD_BYTES (512).
 *
 * Returns EVENTBUS_ERR_QUEUE_FULL if any subscriber's pending queue is full.
 * In that case no delivery is performed (atomic all-or-none publish).
 *
 * Request:  opcode, publisher_pd, payload_shmem_offset, payload_len,
 *           timestamp_ns, topic
 * Reply:    status, delivered_count
 */
typedef struct eventbus_publish_req {
    uint32_t opcode;                        /* EVENTBUS_OP_PUBLISH */
    uint32_t publisher_pd;
    uint32_t payload_shmem_offset;
    uint32_t payload_len;
    uint64_t timestamp_ns;
    char     topic[EVENTBUS_TOPIC_NAME_MAX];
} __attribute__((packed)) eventbus_publish_req_t;

typedef struct eventbus_publish_rep {
    uint32_t status;
    uint32_t delivered_count;
} __attribute__((packed)) eventbus_publish_rep_t;

/*
 * EVENTBUS_OP_DRAIN
 *
 * Retrieve pending events for a subscriber.  Up to EVENTBUS_DRAIN_BATCH_MAX
 * events are written as an array of eventbus_event_t into the caller's shmem
 * MR at buf_shmem_offset.
 *
 * Request:  opcode, subscriber_id, buf_shmem_offset, buf_len
 * Reply:    status, event_count, bytes_written
 */
typedef struct eventbus_drain_req {
    uint32_t opcode;                        /* EVENTBUS_OP_DRAIN */
    uint32_t subscriber_id;
    uint32_t buf_shmem_offset;
    uint32_t buf_len;
} __attribute__((packed)) eventbus_drain_req_t;

typedef struct eventbus_drain_rep {
    uint32_t status;
    uint32_t event_count;
    uint32_t bytes_written;
} __attribute__((packed)) eventbus_drain_rep_t;

/*
 * EVENTBUS_OP_STATUS
 *
 * Return topic count and basic statistics.
 *
 * Request:  opcode
 * Reply:    status, topic_count
 */
typedef struct eventbus_status_req {
    uint32_t opcode;                        /* EVENTBUS_OP_STATUS */
} __attribute__((packed)) eventbus_status_req_t;

typedef struct eventbus_status_rep {
    uint32_t status;
    uint32_t topic_count;
} __attribute__((packed)) eventbus_status_rep_t;

/*
 * EVENTBUS_OP_HEALTH
 *
 * Liveness probe.
 *
 * Request:  opcode
 * Reply:    status, topic_count, version
 */
typedef struct eventbus_health_req {
    uint32_t opcode;                        /* EVENTBUS_OP_HEALTH */
} __attribute__((packed)) eventbus_health_req_t;

typedef struct eventbus_health_rep {
    uint32_t status;
    uint32_t topic_count;
    uint32_t version;                       /* EVENTBUS_INTERFACE_VERSION */
} __attribute__((packed)) eventbus_health_rep_t;
