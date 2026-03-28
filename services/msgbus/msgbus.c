/*
 * agentOS MsgBus — Implementation
 *
 * Core message routing service. All agent communication flows through here.
 * Built on seL4 IPC primitives: endpoints for synchronous messaging,
 * notification objects for async signals, shared memory for bulk data.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <sel4/sel4.h>
#include "msgbus.h"

/*
 * Internal state
 */
static msgbus_channel_desc_t channels[MSGBUS_MAX_CHANNELS];
static msgbus_subscriber_t   subscribers[MSGBUS_MAX_CHANNELS][MSGBUS_MAX_SUBSCRIBERS];
static int                   channel_count = 0;

/*
 * Find channel by name. Returns index or -1.
 */
static int find_channel(const char *name) {
    for (int i = 0; i < channel_count; i++) {
        if (strncmp(channels[i].name, name, MSGBUS_CHANNEL_NAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Find subscriber in a channel. Returns index or -1.
 */
static int find_subscriber(int ch_idx, agent_id_t *agent) {
    for (uint32_t i = 0; i < channels[ch_idx].subscriber_count; i++) {
        if (memcmp(&subscribers[ch_idx][i].agent, agent, sizeof(agent_id_t)) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * Initialize MsgBus
 */
int msgbus_init(void) {
    memset(channels, 0, sizeof(channels));
    memset(subscribers, 0, sizeof(subscribers));
    channel_count = 0;
    
    printf("[msgbus] MsgBus initialized (max %d channels, %d subscribers/channel)\n",
           MSGBUS_MAX_CHANNELS, MSGBUS_MAX_SUBSCRIBERS);
    
    return 0;
}

/*
 * Create well-known system channels
 */
int msgbus_create_system_channels(void) {
    struct {
        const char *name;
        uint32_t flags;
    } sys_channels[] = {
        { MSGBUS_CH_SYSTEM_BROADCAST, MSGBUS_CH_BROADCAST | MSGBUS_CH_SYSTEM },
        { MSGBUS_CH_SYSTEM_EVENTS,    MSGBUS_CH_ORDERED | MSGBUS_CH_SYSTEM },
        { MSGBUS_CH_SYSTEM_LOG,       MSGBUS_CH_PERSISTENT | MSGBUS_CH_SYSTEM },
        { MSGBUS_CH_SYSTEM_HEALTH,    MSGBUS_CH_BROADCAST | MSGBUS_CH_SYSTEM },
        { MSGBUS_CH_TOOLS_REGISTRY,   MSGBUS_CH_ORDERED },
        { MSGBUS_CH_MODELS_REGISTRY,  MSGBUS_CH_ORDERED },
    };
    
    int count = sizeof(sys_channels) / sizeof(sys_channels[0]);
    agent_id_t init_id = {0};  /* Init task has zero ID */
    
    for (int i = 0; i < count; i++) {
        seL4_CPtr ep;
        int err = msgbus_channel_create(&init_id, sys_channels[i].name,
                                         sys_channels[i].flags, &ep);
        if (err) {
            printf("[msgbus] WARNING: Failed to create system channel '%s': %d\n",
                   sys_channels[i].name, err);
        } else {
            printf("[msgbus] System channel '%s' created\n", sys_channels[i].name);
        }
    }
    
    return 0;
}

/*
 * Create a channel
 */
int msgbus_channel_create(agent_id_t *creator, const char *name,
                           uint32_t flags, seL4_CPtr *out_ep) {
    if (channel_count >= MSGBUS_MAX_CHANNELS) {
        return AOS_ERR_NOMEM;
    }
    
    if (find_channel(name) >= 0) {
        return AOS_ERR_EXISTS;
    }
    
    /* System channels can only be created by init task (zero ID) */
    if ((flags & MSGBUS_CH_SYSTEM) != 0) {
        agent_id_t zero = {0};
        if (memcmp(creator, &zero, sizeof(agent_id_t)) != 0) {
            return AOS_ERR_DENIED;
        }
    }
    
    int idx = channel_count++;
    strncpy(channels[idx].name, name, MSGBUS_CHANNEL_NAME_MAX - 1);
    channels[idx].flags = flags;
    channels[idx].subscriber_count = 0;
    channels[idx].message_count = 0;
    memcpy(&channels[idx].owner, creator, sizeof(agent_id_t));
    
    /* TODO: Allocate actual seL4 endpoint via VKA */
    channels[idx].endpoint = 0;
    channels[idx].notification = 0;
    
    if (out_ep) {
        *out_ep = channels[idx].endpoint;
    }
    
    return AOS_OK;
}

/*
 * Delete a channel
 */
int msgbus_channel_delete(agent_id_t *requester, const char *name) {
    int idx = find_channel(name);
    if (idx < 0) return AOS_ERR_NOTFOUND;
    
    /* Only owner or init can delete */
    agent_id_t zero = {0};
    if (memcmp(requester, &channels[idx].owner, sizeof(agent_id_t)) != 0 &&
        memcmp(requester, &zero, sizeof(agent_id_t)) != 0) {
        return AOS_ERR_DENIED;
    }
    
    /* System channels cannot be deleted */
    if (channels[idx].flags & MSGBUS_CH_SYSTEM) {
        return AOS_ERR_DENIED;
    }
    
    /* Remove channel by swapping with last */
    if (idx < channel_count - 1) {
        memcpy(&channels[idx], &channels[channel_count - 1], 
               sizeof(msgbus_channel_desc_t));
        memcpy(&subscribers[idx], &subscribers[channel_count - 1],
               sizeof(msgbus_subscriber_t) * MSGBUS_MAX_SUBSCRIBERS);
    }
    channel_count--;
    
    return AOS_OK;
}

/*
 * Subscribe to a channel
 */
int msgbus_channel_subscribe(agent_id_t *agent, const char *name,
                              seL4_CPtr reply_ep, uint32_t filter) {
    int idx = find_channel(name);
    if (idx < 0) return AOS_ERR_NOTFOUND;
    
    if (channels[idx].subscriber_count >= MSGBUS_MAX_SUBSCRIBERS) {
        return AOS_ERR_NOMEM;
    }
    
    /* Check for exclusive channels */
    if ((channels[idx].flags & MSGBUS_CH_EXCLUSIVE) && 
        channels[idx].subscriber_count > 0) {
        return AOS_ERR_BUSY;
    }
    
    /* Check if already subscribed */
    if (find_subscriber(idx, agent) >= 0) {
        return AOS_ERR_EXISTS;
    }
    
    uint32_t sub_idx = channels[idx].subscriber_count++;
    memcpy(&subscribers[idx][sub_idx].agent, agent, sizeof(agent_id_t));
    subscribers[idx][sub_idx].reply_ep = reply_ep;
    subscribers[idx][sub_idx].badge = sub_idx;  /* Simple badge assignment */
    subscribers[idx][sub_idx].filter = filter;
    
    printf("[msgbus] Agent %02x%02x... subscribed to '%s'\n",
           agent->bytes[0], agent->bytes[1], name);
    
    return AOS_OK;
}

/*
 * Unsubscribe from a channel
 */
int msgbus_channel_unsubscribe(agent_id_t *agent, const char *name) {
    int ch_idx = find_channel(name);
    if (ch_idx < 0) return AOS_ERR_NOTFOUND;
    
    int sub_idx = find_subscriber(ch_idx, agent);
    if (sub_idx < 0) return AOS_ERR_NOTFOUND;
    
    /* Swap with last subscriber */
    uint32_t last = channels[ch_idx].subscriber_count - 1;
    if ((uint32_t)sub_idx < last) {
        memcpy(&subscribers[ch_idx][sub_idx], 
               &subscribers[ch_idx][last],
               sizeof(msgbus_subscriber_t));
    }
    channels[ch_idx].subscriber_count--;
    
    return AOS_OK;
}

/*
 * Publish a message to a channel
 * 
 * For BROADCAST channels: deliver to all subscribers
 * For non-BROADCAST: deliver to the next subscriber (round-robin)
 */
int msgbus_publish(agent_id_t *sender, const char *channel_name,
                    aos_msg_t *msg) {
    int idx = find_channel(channel_name);
    if (idx < 0) return AOS_ERR_NOTFOUND;
    
    if (channels[idx].subscriber_count == 0) {
        /* No subscribers — message is dropped (or queued if persistent) */
        if (channels[idx].flags & MSGBUS_CH_PERSISTENT) {
            /* TODO: Queue message for future subscribers */
            printf("[msgbus] Queued message on '%s' (no subscribers)\n",
                   channel_name);
        }
        return AOS_OK;
    }
    
    channels[idx].message_count++;
    
    if (channels[idx].flags & MSGBUS_CH_BROADCAST) {
        /* Deliver to ALL subscribers */
        for (uint32_t i = 0; i < channels[idx].subscriber_count; i++) {
            /* Skip sending back to the sender */
            if (memcmp(&subscribers[idx][i].agent, sender, sizeof(agent_id_t)) == 0) {
                continue;
            }
            
            /* Check message type filter */
            if (subscribers[idx][i].filter != 0 &&
                !(subscribers[idx][i].filter & (1 << msg->msg_type))) {
                continue;
            }
            
            /* Deliver via seL4 IPC */
            /* TODO: Actual IPC send to subscriber's reply endpoint */
            printf("[msgbus] Delivered to subscriber %d on '%s'\n",
                   i, channel_name);
        }
    } else {
        /* Round-robin delivery */
        /* TODO: Implement proper round-robin with delivery tracking */
        if (channels[idx].subscriber_count > 0) {
            uint32_t target = channels[idx].message_count % 
                              channels[idx].subscriber_count;
            printf("[msgbus] Delivered to subscriber %u on '%s'\n",
                   target, channel_name);
        }
    }
    
    return AOS_OK;
}

/*
 * Send a direct message to a specific agent
 */
int msgbus_send_direct(agent_id_t *sender, agent_id_t *dest,
                        aos_msg_t *msg) {
    /* Look up dest agent's endpoint across all channels */
    /* For direct messaging, we maintain a separate agent registry */
    
    /* TODO: Agent registry lookup */
    printf("[msgbus] Direct message from %02x%02x... to %02x%02x...\n",
           sender->bytes[0], sender->bytes[1],
           dest->bytes[0], dest->bytes[1]);
    
    return AOS_OK;
}

/*
 * Synchronous RPC through MsgBus
 * Caller blocks until callee replies or timeout.
 */
int msgbus_rpc_call(agent_id_t *caller, agent_id_t *callee,
                     aos_msg_t *request, aos_msg_t **reply,
                     uint32_t timeout_ms) {
    /* RPC is implemented as:
     * 1. Send request to callee via seL4_Call (blocks caller)
     * 2. Callee receives via seL4_Recv
     * 3. Callee processes and replies via seL4_Reply
     * 4. Caller unblocks with reply
     *
     * This maps directly to seL4's efficient call/reply IPC path,
     * which is the fastest operation in the kernel (~100 cycles on ARM).
     */
    
    /* TODO: Implement with actual seL4 IPC */
    printf("[msgbus] RPC call from %02x%02x... to %02x%02x... (timeout=%ums)\n",
           caller->bytes[0], caller->bytes[1],
           callee->bytes[0], callee->bytes[1],
           timeout_ms);
    
    *reply = NULL;
    return AOS_OK;
}

/*
 * Main service loop
 * 
 * This runs in MsgBus's own address space as a CAmkES component.
 * It listens on its service endpoint and handles all MsgBus operations.
 */
void msgbus_run(seL4_CPtr service_ep) {
    printf("[msgbus] MsgBus service running on endpoint %lu\n",
           (unsigned long)service_ep);
    
    while (1) {
        seL4_Word sender_badge;
        seL4_MessageInfo_t info;
        
        /* Wait for an IPC request */
        info = seL4_Recv(service_ep, &sender_badge);
        
        seL4_Word op = seL4_MessageInfo_get_label(info);
        
        switch (op) {
            case MSGBUS_OP_CREATE_CHANNEL: {
                /* Extract channel name and flags from message registers */
                /* TODO: Proper parameter extraction */
                printf("[msgbus] CREATE_CHANNEL request from badge=%lu\n",
                       (unsigned long)sender_badge);
                break;
            }
            
            case MSGBUS_OP_SUBSCRIBE: {
                printf("[msgbus] SUBSCRIBE request from badge=%lu\n",
                       (unsigned long)sender_badge);
                break;
            }
            
            case MSGBUS_OP_PUBLISH: {
                printf("[msgbus] PUBLISH request from badge=%lu\n",
                       (unsigned long)sender_badge);
                break;
            }
            
            case MSGBUS_OP_SEND_DIRECT: {
                printf("[msgbus] SEND_DIRECT request from badge=%lu\n",
                       (unsigned long)sender_badge);
                break;
            }
            
            case MSGBUS_OP_CALL_RPC: {
                printf("[msgbus] RPC_CALL request from badge=%lu\n",
                       (unsigned long)sender_badge);
                break;
            }
            
            case MSGBUS_OP_LIST_CHANNELS: {
                printf("[msgbus] LIST_CHANNELS request from badge=%lu\n",
                       (unsigned long)sender_badge);
                /* Reply with channel list */
                seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
                seL4_SetMR(0, (seL4_Word)channel_count);
                seL4_Reply(reply);
                break;
            }
            
            default:
                printf("[msgbus] Unknown operation %lu from badge=%lu\n",
                       (unsigned long)op, (unsigned long)sender_badge);
                break;
        }
    }
}
