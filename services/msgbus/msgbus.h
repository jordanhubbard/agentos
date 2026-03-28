/*
 * agentOS MsgBus — Inter-Agent Communication Service
 *
 * The MsgBus is the nervous system of agentOS. It provides:
 *   - Named channels (pub/sub, point-to-point)
 *   - Agent-to-agent direct messaging
 *   - Multicast groups
 *   - Message queue persistence
 *   - Capability-gated access
 *
 * Built on seL4 endpoints, notification objects, and shared memory.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef AGENTOS_MSGBUS_H
#define AGENTOS_MSGBUS_H

#include <stdint.h>
#include <agentOS.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum limits
 */
#define MSGBUS_MAX_CHANNELS     256
#define MSGBUS_MAX_SUBSCRIBERS  64      /* Per channel */
#define MSGBUS_MAX_QUEUE_DEPTH  1024    /* Messages per channel */
#define MSGBUS_MAX_MSG_SIZE     (64 * 1024)  /* 64KB inline message */
#define MSGBUS_CHANNEL_NAME_MAX 128

/*
 * Channel flags
 */
#define MSGBUS_CH_PERSISTENT    (1 << 0)  /* Messages survive agent restart */
#define MSGBUS_CH_ORDERED       (1 << 1)  /* Guaranteed ordering */
#define MSGBUS_CH_BROADCAST     (1 << 2)  /* All subscribers get every message */
#define MSGBUS_CH_EXCLUSIVE     (1 << 3)  /* Only one subscriber at a time */
#define MSGBUS_CH_SYSTEM        (1 << 7)  /* System channel (init task only) */

/*
 * Well-known system channels
 */
#define MSGBUS_CH_SYSTEM_BROADCAST  "system.broadcast"
#define MSGBUS_CH_SYSTEM_EVENTS     "system.events"
#define MSGBUS_CH_SYSTEM_LOG        "system.log"
#define MSGBUS_CH_SYSTEM_HEALTH     "system.health"
#define MSGBUS_CH_TOOLS_REGISTRY    "tools.registry"
#define MSGBUS_CH_MODELS_REGISTRY   "models.registry"

/*
 * MsgBus operations (IPC labels)
 */
typedef enum {
    MSGBUS_OP_CREATE_CHANNEL    = 0x100,
    MSGBUS_OP_DELETE_CHANNEL    = 0x101,
    MSGBUS_OP_SUBSCRIBE         = 0x102,
    MSGBUS_OP_UNSUBSCRIBE       = 0x103,
    MSGBUS_OP_PUBLISH           = 0x104,
    MSGBUS_OP_SEND_DIRECT       = 0x105,
    MSGBUS_OP_RECV              = 0x106,
    MSGBUS_OP_LIST_CHANNELS     = 0x107,
    MSGBUS_OP_CHANNEL_INFO      = 0x108,
    MSGBUS_OP_CALL_RPC          = 0x109,  /* Synchronous call */
    MSGBUS_OP_REPLY_RPC         = 0x10A,  /* RPC reply */
} msgbus_op_t;

/*
 * Channel descriptor (internal)
 */
typedef struct {
    char            name[MSGBUS_CHANNEL_NAME_MAX];
    uint32_t        flags;
    uint32_t        subscriber_count;
    uint32_t        message_count;
    agent_id_t      owner;          /* Creator of the channel */
    seL4_CPtr       endpoint;       /* seL4 endpoint for this channel */
    seL4_CPtr       notification;   /* seL4 notification for async delivery */
} msgbus_channel_desc_t;

/*
 * Subscriber entry
 */
typedef struct {
    agent_id_t      agent;
    seL4_CPtr       reply_ep;   /* Agent's reply endpoint */
    uint32_t        badge;      /* Badge for identification */
    uint32_t        filter;     /* Message type filter bitmask */
} msgbus_subscriber_t;

/*
 * Queued message (internal ring buffer entry)
 */
typedef struct {
    aos_msg_t       header;
    uint64_t        enqueue_time;   /* Timestamp when queued */
    uint32_t        delivery_count; /* Times delivered (for retry) */
    /* Payload follows header (flexible array in actual impl) */
} msgbus_queued_msg_t;

/*
 * Service interface functions
 */

/* Initialize MsgBus service */
int msgbus_init(void);

/* Create system channels (called during boot) */
int msgbus_create_system_channels(void);

/* Main service loop (blocks, handles IPC requests) */
void msgbus_run(seL4_CPtr service_ep);

/* Channel operations */
int msgbus_channel_create(agent_id_t *creator, const char *name, 
                           uint32_t flags, seL4_CPtr *out_ep);
int msgbus_channel_delete(agent_id_t *requester, const char *name);
int msgbus_channel_subscribe(agent_id_t *agent, const char *name,
                              seL4_CPtr reply_ep, uint32_t filter);
int msgbus_channel_unsubscribe(agent_id_t *agent, const char *name);

/* Message operations */
int msgbus_publish(agent_id_t *sender, const char *channel,
                    aos_msg_t *msg);
int msgbus_send_direct(agent_id_t *sender, agent_id_t *dest,
                        aos_msg_t *msg);
int msgbus_deliver_pending(agent_id_t *agent);

/* RPC (synchronous call/reply through MsgBus) */
int msgbus_rpc_call(agent_id_t *caller, agent_id_t *callee,
                     aos_msg_t *request, aos_msg_t **reply,
                     uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AGENTOS_MSGBUS_H */
