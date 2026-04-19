/*
 * agentOS MsgBus IPC Contract — interface.h
 *
 * Formal seL4 IPC API contract for the MsgBus protection domain.
 * MsgBus is the message routing backbone of agentOS.  All inter-agent
 * communication flows through this passive server (see topology.yaml
 * for the PD definition; this service is embedded in the event_bus PD
 * for system-channel traffic and exposed to agents via the controller).
 *
 * IPC mechanism: seL4_Call / seL4_Reply over badged endpoints.
 * Label field (seL4_MessageInfo label) carries the opcode.
 * MR0 is always the opcode on request; MR0 is always the status on reply.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define MSGBUS_INTERFACE_VERSION  1

/* ── Limits (informational — enforced by the server) ────────────────────── */

#define MSGBUS_MAX_CHANNELS       256
#define MSGBUS_MAX_SUBSCRIBERS    64
#define MSGBUS_MAX_QUEUE_DEPTH    1024
#define MSGBUS_MAX_MSG_SIZE       65536   /* 64KB max inline payload */
#define MSGBUS_CHANNEL_NAME_MAX   128

/* ── Channel flags (used in CREATE_CHANNEL requests) ────────────────────── */

#define MSGBUS_CH_PERSISTENT    (1u << 0)  /* messages survive agent restart */
#define MSGBUS_CH_ORDERED       (1u << 1)  /* guaranteed ordering */
#define MSGBUS_CH_BROADCAST     (1u << 2)  /* all subscribers receive every msg */
#define MSGBUS_CH_EXCLUSIVE     (1u << 3)  /* only one subscriber at a time */
#define MSGBUS_CH_SYSTEM        (1u << 7)  /* system-only; init task creation */

/* ── Well-known channel name constants ───────────────────────────────────── */

#define MSGBUS_CH_SYSTEM_BROADCAST  "system.broadcast"
#define MSGBUS_CH_SYSTEM_EVENTS     "system.events"
#define MSGBUS_CH_SYSTEM_LOG        "system.log"
#define MSGBUS_CH_SYSTEM_HEALTH     "system.health"
#define MSGBUS_CH_TOOLS_REGISTRY    "tools.registry"
#define MSGBUS_CH_MODELS_REGISTRY   "models.registry"

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

#define MSGBUS_OP_CREATE_CHANNEL    0x100u
#define MSGBUS_OP_DELETE_CHANNEL    0x101u
#define MSGBUS_OP_SUBSCRIBE         0x102u
#define MSGBUS_OP_UNSUBSCRIBE       0x103u
#define MSGBUS_OP_PUBLISH           0x104u
#define MSGBUS_OP_SEND_DIRECT       0x105u
#define MSGBUS_OP_RECV              0x106u
#define MSGBUS_OP_LIST_CHANNELS     0x107u
#define MSGBUS_OP_CHANNEL_INFO      0x108u
#define MSGBUS_OP_CALL_RPC          0x109u  /* synchronous call through bus */
#define MSGBUS_OP_REPLY_RPC         0x10Au  /* RPC reply */
#define MSGBUS_OP_HEALTH            0x10Bu  /* liveness probe */

/* ── Error codes (MR0 in all replies) ───────────────────────────────────── */

#define MSGBUS_ERR_OK               0u   /* success */
#define MSGBUS_ERR_INVALID_ARG      1u   /* bad opcode, null pointer, etc. */
#define MSGBUS_ERR_NOT_FOUND        2u   /* channel or agent does not exist */
#define MSGBUS_ERR_EXISTS           3u   /* channel name already taken */
#define MSGBUS_ERR_DENIED           4u   /* caller lacks required capability */
#define MSGBUS_ERR_NOMEM            5u   /* channel table or queue is full */
#define MSGBUS_ERR_BUSY             6u   /* exclusive channel already subscribed */
#define MSGBUS_ERR_TIMEOUT          7u   /* RPC call timed out */
#define MSGBUS_ERR_MSG_TOO_BIG      8u   /* payload exceeds MSGBUS_MAX_MSG_SIZE */
#define MSGBUS_ERR_INTERNAL         99u  /* unexpected server error */

/* ── Request and reply structs ───────────────────────────────────────────── */

/*
 * MSGBUS_OP_CREATE_CHANNEL
 *
 * Create a named channel.  System channels (MSGBUS_CH_SYSTEM) may only be
 * created by the init task (badge == 0).
 *
 * Request:  opcode, flags, name (NUL-terminated in name[])
 * Reply:    status
 */
typedef struct msgbus_create_channel_req {
    uint32_t opcode;                        /* MSGBUS_OP_CREATE_CHANNEL */
    uint32_t flags;                         /* MSGBUS_CH_* bitmask */
    char     name[MSGBUS_CHANNEL_NAME_MAX]; /* NUL-terminated channel name */
} __attribute__((packed)) msgbus_create_channel_req_t;

typedef struct msgbus_create_channel_rep {
    uint32_t status;                        /* MSGBUS_ERR_* */
} __attribute__((packed)) msgbus_create_channel_rep_t;

/*
 * MSGBUS_OP_DELETE_CHANNEL
 *
 * Delete a channel.  Only the owner or init task may delete.
 * System channels cannot be deleted.
 *
 * Request:  opcode, name
 * Reply:    status
 */
typedef struct msgbus_delete_channel_req {
    uint32_t opcode;                        /* MSGBUS_OP_DELETE_CHANNEL */
    uint32_t _pad;
    char     name[MSGBUS_CHANNEL_NAME_MAX];
} __attribute__((packed)) msgbus_delete_channel_req_t;

typedef struct msgbus_delete_channel_rep {
    uint32_t status;
} __attribute__((packed)) msgbus_delete_channel_rep_t;

/*
 * MSGBUS_OP_SUBSCRIBE
 *
 * Subscribe to a channel.  filter is a bitmask of message types to accept;
 * 0 means "accept all".  Returns a badge the subscriber can use to identify
 * itself in subsequent RECV calls.
 *
 * Request:  opcode, filter, name
 * Reply:    status, subscriber_badge
 */
typedef struct msgbus_subscribe_req {
    uint32_t opcode;                        /* MSGBUS_OP_SUBSCRIBE */
    uint32_t filter;                        /* message type bitmask; 0 = all */
    char     name[MSGBUS_CHANNEL_NAME_MAX];
} __attribute__((packed)) msgbus_subscribe_req_t;

typedef struct msgbus_subscribe_rep {
    uint32_t status;
    uint32_t subscriber_badge;              /* opaque handle for RECV/UNSUBSCRIBE */
} __attribute__((packed)) msgbus_subscribe_rep_t;

/*
 * MSGBUS_OP_UNSUBSCRIBE
 *
 * Remove the calling agent's subscription from a channel.
 *
 * Request:  opcode, subscriber_badge, name
 * Reply:    status
 */
typedef struct msgbus_unsubscribe_req {
    uint32_t opcode;                        /* MSGBUS_OP_UNSUBSCRIBE */
    uint32_t subscriber_badge;
    char     name[MSGBUS_CHANNEL_NAME_MAX];
} __attribute__((packed)) msgbus_unsubscribe_req_t;

typedef struct msgbus_unsubscribe_rep {
    uint32_t status;
} __attribute__((packed)) msgbus_unsubscribe_rep_t;

/*
 * MSGBUS_OP_PUBLISH
 *
 * Publish a message to a named channel.
 * For BROADCAST channels all subscribers receive the message.
 * For non-BROADCAST channels delivery is round-robin.
 * payload_len must not exceed MSGBUS_MAX_MSG_SIZE.
 *
 * The payload is placed in a shared memory region whose offset and length
 * are encoded in the request.  The server reads from that region atomically
 * before replying.
 *
 * Request:  opcode, msg_type, payload_shmem_offset, payload_len, channel_name
 * Reply:    status, delivered_count
 */
typedef struct msgbus_publish_req {
    uint32_t opcode;                        /* MSGBUS_OP_PUBLISH */
    uint32_t msg_type;                      /* caller-defined message type tag */
    uint32_t payload_shmem_offset;          /* offset into caller's shmem MR */
    uint32_t payload_len;                   /* bytes of payload */
    char     channel[MSGBUS_CHANNEL_NAME_MAX];
} __attribute__((packed)) msgbus_publish_req_t;

typedef struct msgbus_publish_rep {
    uint32_t status;
    uint32_t delivered_count;               /* number of subscribers notified */
} __attribute__((packed)) msgbus_publish_rep_t;

/*
 * MSGBUS_OP_SEND_DIRECT
 *
 * Send a message directly to a specific agent (point-to-point, no channel).
 * dest_badge is the seL4 badge of the destination agent's endpoint.
 *
 * Request:  opcode, dest_badge, msg_type, payload_shmem_offset, payload_len
 * Reply:    status
 */
typedef struct msgbus_send_direct_req {
    uint32_t opcode;                        /* MSGBUS_OP_SEND_DIRECT */
    uint32_t dest_badge;
    uint32_t msg_type;
    uint32_t payload_shmem_offset;
    uint32_t payload_len;
} __attribute__((packed)) msgbus_send_direct_req_t;

typedef struct msgbus_send_direct_rep {
    uint32_t status;
} __attribute__((packed)) msgbus_send_direct_rep_t;

/*
 * MSGBUS_OP_RECV
 *
 * Receive the next pending message for the calling subscriber.
 * Returns MSGBUS_ERR_NOT_FOUND (with msg_type=0, payload_len=0) if the
 * queue is empty.  The payload is written into the caller's shmem MR.
 *
 * Request:  opcode, subscriber_badge, buf_shmem_offset, buf_len, channel_name
 * Reply:    status, msg_type, payload_shmem_offset, payload_len
 */
typedef struct msgbus_recv_req {
    uint32_t opcode;                        /* MSGBUS_OP_RECV */
    uint32_t subscriber_badge;
    uint32_t buf_shmem_offset;              /* where to write the payload */
    uint32_t buf_len;                       /* max bytes to write */
    char     channel[MSGBUS_CHANNEL_NAME_MAX];
} __attribute__((packed)) msgbus_recv_req_t;

typedef struct msgbus_recv_rep {
    uint32_t status;
    uint32_t msg_type;
    uint32_t payload_shmem_offset;
    uint32_t payload_len;
} __attribute__((packed)) msgbus_recv_rep_t;

/*
 * MSGBUS_OP_LIST_CHANNELS
 *
 * Return the number of active channels.  The channel name list is written
 * into the caller's shmem MR starting at buf_shmem_offset, as a packed
 * array of NUL-terminated strings.
 *
 * Request:  opcode, buf_shmem_offset, buf_len
 * Reply:    status, channel_count, bytes_written
 */
typedef struct msgbus_list_channels_req {
    uint32_t opcode;                        /* MSGBUS_OP_LIST_CHANNELS */
    uint32_t buf_shmem_offset;
    uint32_t buf_len;
} __attribute__((packed)) msgbus_list_channels_req_t;

typedef struct msgbus_list_channels_rep {
    uint32_t status;
    uint32_t channel_count;
    uint32_t bytes_written;
} __attribute__((packed)) msgbus_list_channels_rep_t;

/*
 * MSGBUS_OP_CALL_RPC
 *
 * Synchronous RPC through the bus.  The caller blocks until the callee
 * replies via MSGBUS_OP_REPLY_RPC or timeout_ms elapses.
 * callee_badge is the seL4 badge of the target agent.
 *
 * Request:  opcode, callee_badge, timeout_ms, req_shmem_offset, req_len
 * Reply:    status, rep_shmem_offset, rep_len
 */
typedef struct msgbus_call_rpc_req {
    uint32_t opcode;                        /* MSGBUS_OP_CALL_RPC */
    uint32_t callee_badge;
    uint32_t timeout_ms;
    uint32_t req_shmem_offset;
    uint32_t req_len;
} __attribute__((packed)) msgbus_call_rpc_req_t;

typedef struct msgbus_call_rpc_rep {
    uint32_t status;
    uint32_t rep_shmem_offset;
    uint32_t rep_len;
} __attribute__((packed)) msgbus_call_rpc_rep_t;

/*
 * MSGBUS_OP_HEALTH
 *
 * Liveness probe.  Always returns MSGBUS_ERR_OK when the server is live.
 *
 * Request:  opcode
 * Reply:    status, channel_count, version
 */
typedef struct msgbus_health_req {
    uint32_t opcode;                        /* MSGBUS_OP_HEALTH */
} __attribute__((packed)) msgbus_health_req_t;

typedef struct msgbus_health_rep {
    uint32_t status;
    uint32_t channel_count;
    uint32_t version;                       /* MSGBUS_INTERFACE_VERSION */
} __attribute__((packed)) msgbus_health_rep_t;
