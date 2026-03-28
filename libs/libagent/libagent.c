/*
 * agentOS libagent — Agent SDK Implementation
 *
 * This implements the libagent API declared in agentOS.h.
 * Agents link against this to get the full agentOS SDK.
 *
 * IPC model: each API call maps to a seL4_Call into the appropriate
 * system service (MsgBus, CapStore, MemFS, ModelSvc, ToolSvc).
 *
 * The service endpoint capabilities are set up at agent initialization
 * by the init task, which grants each agent a fixed-slot CSpace layout:
 *
 *   SLOT 1: MsgBus endpoint
 *   SLOT 2: CapStore endpoint
 *   SLOT 3: MemFS endpoint
 *   SLOT 4: ModelSvc endpoint
 *   SLOT 5: ToolSvc endpoint
 *   SLOT 6: LogSvc endpoint
 *   SLOT 7: Reply capability (for synchronous calls)
 *   SLOT 8+: Agent-allocated capabilities
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>
#include <stdlib.h>
#include "agentOS.h"

/* 
 * Service endpoint slots in our CSpace.
 * These are granted by the init task at spawn time.
 */
#define SLOT_MSGBUS    1
#define SLOT_CAPSTORE  2
#define SLOT_MEMFS     3
#define SLOT_MODELSVC  4
#define SLOT_TOOLSVC   5
#define SLOT_LOGSVC    6
#define SLOT_REPLY     7

/* IPC message registers (seL4 uses MR0..MR3 for small payloads) */
#define MR_OP    0   /* operation code */
#define MR_ARG0  1   /* first argument */
#define MR_ARG1  2   /* second argument */
#define MR_ARG2  3   /* third argument */

/* Agent global state */
static struct {
    bool initialized;
    agent_id_t id;
    char name[64];
    aos_config_t config;
    uint64_t seqno;
} agent_state = { .initialized = false };

/* =========================================================================
 * Internal IPC helpers
 * =========================================================================*/

/*
 * Synchronous call into a service endpoint.
 * Packs op + up to 3 uint64 args into message registers.
 * Returns the response label (status code).
 */
static inline seL4_MessageInfo_t
ipc_call(seL4_CPtr endpoint, uint64_t op,
         uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
    seL4_SetMR(MR_OP,   op);
    seL4_SetMR(MR_ARG0, arg0);
    seL4_SetMR(MR_ARG1, arg1);
    seL4_SetMR(MR_ARG2, arg2);
    return seL4_Call(endpoint, seL4_MessageInfo_new(op, 0, 0, 4));
}

/*
 * Call with a pointer payload — address + length packed into args.
 * For payloads > 24 bytes, the caller should use shared memory instead.
 */
static inline seL4_MessageInfo_t
ipc_call_ptr(seL4_CPtr endpoint, uint64_t op,
             const void *ptr, size_t len, uint64_t extra)
{
    seL4_SetMR(MR_OP,   op);
    seL4_SetMR(MR_ARG0, (uint64_t)(uintptr_t)ptr);
    seL4_SetMR(MR_ARG1, (uint64_t)len);
    seL4_SetMR(MR_ARG2, extra);
    return seL4_Call(endpoint, seL4_MessageInfo_new(op, 0, 0, 4));
}

/* =========================================================================
 * Lifecycle
 * =========================================================================*/

aos_status_t aos_init(aos_config_t *config) {
    if (agent_state.initialized) {
        return AOS_OK; /* idempotent */
    }
    if (!config || !config->agent_name) {
        return AOS_ERR_INVALID;
    }
    
    memcpy(&agent_state.config, config, sizeof(aos_config_t));
    strncpy(agent_state.name, config->agent_name, sizeof(agent_state.name) - 1);
    agent_state.seqno = 0;
    
    /* 
     * In a real seL4 system: verify our service endpoint slots are valid.
     * For now, trust that the init task set up our CSpace correctly.
     */
    
    /* Announce ourselves to the MsgBus */
    seL4_MessageInfo_t reply = ipc_call(
        SLOT_MSGBUS,
        0x001, /* MSGBUS_OP_AGENT_REGISTER */
        (uint64_t)(uintptr_t)agent_state.name,
        strlen(agent_state.name),
        0
    );
    
    int status = (int)seL4_MessageInfo_get_label(reply);
    if (status != 0) {
        /* Non-fatal: maybe MsgBus isn't up yet in early boot */
        /* Continue anyway - agent can function without announcement */
    }
    
    agent_state.initialized = true;
    return AOS_OK;
}

aos_status_t aos_shutdown(void) {
    if (!agent_state.initialized) return AOS_OK;
    
    /* Notify MsgBus we're going away */
    ipc_call(SLOT_MSGBUS, 0x002 /* AGENT_DEREGISTER */, 0, 0, 0);
    
    agent_state.initialized = false;
    return AOS_OK;
}

/* =========================================================================
 * Messaging — Channel API
 * =========================================================================*/

#define MSGBUS_OP_CREATE  0x100
#define MSGBUS_OP_OPEN    0x101
#define MSGBUS_OP_SUB     0x102
#define MSGBUS_OP_SEND    0x110
#define MSGBUS_OP_PUBLISH 0x111
#define MSGBUS_OP_RECV    0x112
#define MSGBUS_OP_CALL    0x113

aos_channel_t aos_channel_create(const char *name, uint32_t flags) {
    if (!name) return 0;
    
    seL4_MessageInfo_t reply = ipc_call_ptr(
        SLOT_MSGBUS, MSGBUS_OP_CREATE,
        name, strlen(name), flags
    );
    
    /* Returns the channel handle in MR0 */
    return (aos_channel_t)seL4_GetMR(0);
}

aos_channel_t aos_channel_open(const char *name) {
    if (!name) return 0;
    
    seL4_MessageInfo_t reply = ipc_call_ptr(
        SLOT_MSGBUS, MSGBUS_OP_OPEN,
        name, strlen(name), 0
    );
    
    return (aos_channel_t)seL4_GetMR(0);
}

aos_status_t aos_channel_subscribe(aos_channel_t ch) {
    seL4_MessageInfo_t reply = ipc_call(
        SLOT_MSGBUS, MSGBUS_OP_SUB,
        (uint64_t)ch, 0, 0
    );
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_msg_send(agent_id_t dest, aos_msg_t *msg) {
    if (!msg) return AOS_ERR_INVALID;
    
    /* Pack: destination (32 bytes of agent_id) + msg header + payload */
    /* For v0.1: pass pointer + length to MsgBus which copies it */
    
    seL4_SetMR(0, MSGBUS_OP_SEND);
    seL4_SetMR(1, (uint64_t)(uintptr_t)dest.bytes);  /* dest id ptr */
    seL4_SetMR(2, (uint64_t)(uintptr_t)msg);          /* msg ptr */
    seL4_SetMR(3, (uint64_t)msg->payload_len);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_MSGBUS,
        seL4_MessageInfo_new(MSGBUS_OP_SEND, 0, 0, 4)
    );
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_msg_publish(aos_channel_t ch, aos_msg_t *msg) {
    if (!msg) return AOS_ERR_INVALID;
    
    seL4_SetMR(0, MSGBUS_OP_PUBLISH);
    seL4_SetMR(1, (uint64_t)ch);
    seL4_SetMR(2, (uint64_t)(uintptr_t)msg);
    seL4_SetMR(3, (uint64_t)msg->payload_len);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_MSGBUS,
        seL4_MessageInfo_new(MSGBUS_OP_PUBLISH, 0, 0, 4)
    );
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_msg_t *aos_msg_recv(aos_channel_t ch, uint32_t timeout_ms) {
    /* Allocate a receive buffer */
    aos_msg_t *msg = (aos_msg_t *)malloc(sizeof(aos_msg_t) + 4096);
    if (!msg) return NULL;
    
    seL4_SetMR(0, MSGBUS_OP_RECV);
    seL4_SetMR(1, (uint64_t)ch);
    seL4_SetMR(2, (uint64_t)(uintptr_t)msg);
    seL4_SetMR(3, (uint64_t)timeout_ms);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_MSGBUS,
        seL4_MessageInfo_new(MSGBUS_OP_RECV, 0, 0, 4)
    );
    
    int status = (int)seL4_MessageInfo_get_label(reply);
    if (status != 0) {
        free(msg);
        return NULL;
    }
    return msg;
}

aos_msg_t *aos_msg_call(agent_id_t dest, aos_msg_t *request, uint32_t timeout_ms) {
    if (!request) return NULL;
    
    aos_msg_t *response = (aos_msg_t *)malloc(sizeof(aos_msg_t) + 4096);
    if (!response) return NULL;
    
    seL4_SetMR(0, MSGBUS_OP_CALL);
    seL4_SetMR(1, (uint64_t)(uintptr_t)dest.bytes);
    seL4_SetMR(2, (uint64_t)(uintptr_t)request);
    seL4_SetMR(3, (uint64_t)(uintptr_t)response);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_MSGBUS,
        seL4_MessageInfo_new(MSGBUS_OP_CALL, 0, 0, 4)
    );
    
    int status = (int)seL4_MessageInfo_get_label(reply);
    if (status != 0) {
        free(response);
        return NULL;
    }
    return response;
}

void aos_msg_free(aos_msg_t *msg) {
    free(msg);
}

aos_msg_t *aos_msg_alloc(aos_msg_type_t type, size_t payload_len) {
    aos_msg_t *msg = (aos_msg_t *)malloc(sizeof(aos_msg_t) + payload_len);
    if (!msg) return NULL;
    
    memset(msg, 0, sizeof(aos_msg_t));
    msg->type = type;
    msg->payload_len = payload_len;
    agent_state.seqno++;
    msg->seq = agent_state.seqno;
    return msg;
}

/* =========================================================================
 * Tool Registry
 * =========================================================================*/

#define TOOLSVC_OP_REGISTER   0x200
#define TOOLSVC_OP_UNREGISTER 0x201
#define TOOLSVC_OP_CALL       0x202
#define TOOLSVC_OP_LIST       0x203

aos_status_t aos_tool_register(aos_tool_def_t *def) {
    if (!def || !def->name) return AOS_ERR_INVALID;
    
    seL4_MessageInfo_t reply = ipc_call_ptr(
        SLOT_TOOLSVC, TOOLSVC_OP_REGISTER,
        def, sizeof(aos_tool_def_t), 0
    );
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_tool_unregister(const char *name) {
    if (!name) return AOS_ERR_INVALID;
    
    seL4_MessageInfo_t reply = ipc_call_ptr(
        SLOT_TOOLSVC, TOOLSVC_OP_UNREGISTER,
        name, strlen(name), 0
    );
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_tool_call(aos_tool_cap_t cap, const char *method,
                            const uint8_t *input, size_t input_len,
                            uint8_t **output, size_t *output_len) {
    if (!method || !output || !output_len) return AOS_ERR_INVALID;
    
    /* Allocate output buffer */
    uint8_t *out = (uint8_t *)malloc(65536);
    if (!out) return AOS_ERR_NOMEM;
    
    seL4_SetMR(0, TOOLSVC_OP_CALL);
    seL4_SetMR(1, (uint64_t)cap);
    seL4_SetMR(2, (uint64_t)(uintptr_t)input);
    seL4_SetMR(3, (uint64_t)input_len);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_TOOLSVC,
        seL4_MessageInfo_new(TOOLSVC_OP_CALL, 0, 0, 4)
    );
    
    int status = (int)seL4_MessageInfo_get_label(reply);
    if (status != 0) {
        free(out);
        return (aos_status_t)status;
    }
    
    *output = out;
    *output_len = (size_t)seL4_GetMR(0);
    return AOS_OK;
}

aos_status_t aos_tool_list(char ***names, size_t *count) {
    if (!names || !count) return AOS_ERR_INVALID;
    
    seL4_MessageInfo_t reply = ipc_call(SLOT_TOOLSVC, TOOLSVC_OP_LIST, 0, 0, 0);
    
    *count = (size_t)seL4_GetMR(0);
    /* Tool names are returned in shared memory - pointer in MR1 */
    *names = (char **)(uintptr_t)seL4_GetMR(1);
    
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

/* =========================================================================
 * Model/Inference API
 * =========================================================================*/

#define MODELSVC_OP_QUERY  0x300
#define MODELSVC_OP_LIST   0x301

aos_inference_t aos_model_query(aos_model_cap_t cap, const char *prompt,
                                 const uint8_t *context, size_t context_len) {
    aos_inference_t result = { .text = NULL, .len = 0, .tokens = 0, .status = AOS_ERR_INVALID };
    
    if (!prompt) return result;
    
    /* Allocate response buffer (up to 64KB) */
    char *out = (char *)malloc(65536);
    if (!out) {
        result.status = AOS_ERR_NOMEM;
        return result;
    }
    
    seL4_SetMR(0, MODELSVC_OP_QUERY);
    seL4_SetMR(1, (uint64_t)cap);
    seL4_SetMR(2, (uint64_t)(uintptr_t)prompt);
    seL4_SetMR(3, (uint64_t)(uintptr_t)out);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_MODELSVC,
        seL4_MessageInfo_new(MODELSVC_OP_QUERY, 0, 0, 4)
    );
    
    result.status = (aos_status_t)seL4_MessageInfo_get_label(reply);
    if (result.status == AOS_OK) {
        result.text   = out;
        result.len    = (size_t)seL4_GetMR(0);
        result.tokens = (uint32_t)seL4_GetMR(1);
    } else {
        free(out);
        result.text = NULL;
    }
    return result;
}

void aos_inference_free(aos_inference_t *result) {
    if (result && result->text) {
        free(result->text);
        result->text = NULL;
        result->len = 0;
    }
}

aos_status_t aos_model_list(char ***model_ids, size_t *count) {
    if (!model_ids || !count) return AOS_ERR_INVALID;
    
    seL4_MessageInfo_t reply = ipc_call(SLOT_MODELSVC, MODELSVC_OP_LIST, 0, 0, 0);
    *count = (size_t)seL4_GetMR(0);
    *model_ids = (char **)(uintptr_t)seL4_GetMR(1);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

/* =========================================================================
 * Object Store (MemFS)
 * =========================================================================*/

#define MEMFS_OP_WRITE  0x400
#define MEMFS_OP_READ   0x401
#define MEMFS_OP_CLOSE  0x402
#define MEMFS_OP_LIST   0x403
#define MEMFS_OP_DELETE 0x404

aos_store_t aos_store_open(cap_t store_cap, const char *path, uint32_t flags) {
    if (!path) return 0;
    
    seL4_SetMR(0, MEMFS_OP_WRITE);
    seL4_SetMR(1, store_cap);
    seL4_SetMR(2, (uint64_t)(uintptr_t)path);
    seL4_SetMR(3, flags);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_MEMFS,
        seL4_MessageInfo_new(MEMFS_OP_WRITE, 0, 0, 4)
    );
    return (aos_store_t)seL4_GetMR(0);
}

aos_status_t aos_store_write(aos_store_t h, const void *buf, size_t len, size_t *written) {
    seL4_SetMR(0, MEMFS_OP_WRITE);
    seL4_SetMR(1, (uint64_t)h);
    seL4_SetMR(2, (uint64_t)(uintptr_t)buf);
    seL4_SetMR(3, (uint64_t)len);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_MEMFS,
        seL4_MessageInfo_new(MEMFS_OP_WRITE, 0, 0, 4)
    );
    
    if (written) *written = (size_t)seL4_GetMR(0);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_store_read(aos_store_t h, void *buf, size_t len, size_t *nread) {
    seL4_SetMR(0, MEMFS_OP_READ);
    seL4_SetMR(1, (uint64_t)h);
    seL4_SetMR(2, (uint64_t)(uintptr_t)buf);
    seL4_SetMR(3, (uint64_t)len);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_MEMFS,
        seL4_MessageInfo_new(MEMFS_OP_READ, 0, 0, 4)
    );
    
    if (nread) *nread = (size_t)seL4_GetMR(0);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_store_close(aos_store_t h) {
    seL4_MessageInfo_t reply = ipc_call(SLOT_MEMFS, MEMFS_OP_CLOSE, h, 0, 0);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_store_list(cap_t store_cap, const char *path,
                             char ***entries, size_t *count) {
    if (!path || !entries || !count) return AOS_ERR_INVALID;
    
    seL4_MessageInfo_t reply = ipc_call_ptr(
        SLOT_MEMFS, MEMFS_OP_LIST,
        path, strlen(path), store_cap
    );
    
    *count = (size_t)seL4_GetMR(0);
    *entries = (char **)(uintptr_t)seL4_GetMR(1);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_store_delete(cap_t store_cap, const char *path) {
    if (!path) return AOS_ERR_INVALID;
    return (aos_status_t)seL4_MessageInfo_get_label(
        ipc_call_ptr(SLOT_MEMFS, MEMFS_OP_DELETE, path, strlen(path), store_cap)
    );
}

/* =========================================================================
 * Capability Management
 * =========================================================================*/

#define CAPSTORE_OP_DERIVE  0x500
#define CAPSTORE_OP_GRANT   0x501
#define CAPSTORE_OP_REVOKE  0x502
#define CAPSTORE_OP_LIST    0x503
#define CAPSTORE_OP_INFO    0x504

cap_t aos_cap_derive(cap_t parent, aos_rights_t rights) {
    seL4_MessageInfo_t reply = ipc_call(SLOT_CAPSTORE, CAPSTORE_OP_DERIVE, parent, rights, 0);
    if (seL4_MessageInfo_get_label(reply) != 0) return AOS_CAP_NULL;
    return (cap_t)seL4_GetMR(0);
}

aos_status_t aos_cap_grant(agent_id_t dest, cap_t cap) {
    seL4_SetMR(0, CAPSTORE_OP_GRANT);
    seL4_SetMR(1, (uint64_t)(uintptr_t)dest.bytes);
    seL4_SetMR(2, cap);
    seL4_SetMR(3, 0);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_CAPSTORE,
        seL4_MessageInfo_new(CAPSTORE_OP_GRANT, 0, 0, 4)
    );
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_cap_revoke(cap_t cap) {
    seL4_MessageInfo_t reply = ipc_call(SLOT_CAPSTORE, CAPSTORE_OP_REVOKE, cap, 0, 0);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_cap_list(cap_t **caps, size_t *count) {
    if (!caps || !count) return AOS_ERR_INVALID;
    seL4_MessageInfo_t reply = ipc_call(SLOT_CAPSTORE, CAPSTORE_OP_LIST, 0, 0, 0);
    *count = (size_t)seL4_GetMR(0);
    *caps = (cap_t *)(uintptr_t)seL4_GetMR(1);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_cap_info(cap_t cap, uint32_t *type, aos_rights_t *rights) {
    seL4_MessageInfo_t reply = ipc_call(SLOT_CAPSTORE, CAPSTORE_OP_INFO, cap, 0, 0);
    if (type)   *type   = (uint32_t)seL4_GetMR(0);
    if (rights) *rights = (aos_rights_t)seL4_GetMR(1);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

/* =========================================================================
 * Vibe-Coding: Service Proposal & Hot-Swap
 * =========================================================================*/

#define VIBE_OP_PROPOSE 0x700
#define VIBE_OP_STATUS  0x701
#define VIBE_OP_SWAP    0x702

aos_status_t aos_service_propose(const char *service_id,
                                  const char *new_impl_path,
                                  uint32_t *proposal_id) {
    if (!service_id || !new_impl_path || !proposal_id) return AOS_ERR_INVALID;
    
    seL4_SetMR(0, VIBE_OP_PROPOSE);
    seL4_SetMR(1, (uint64_t)(uintptr_t)service_id);
    seL4_SetMR(2, (uint64_t)(uintptr_t)new_impl_path);
    seL4_SetMR(3, 0);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_TOOLSVC,  /* ToolSvc handles vibe proposals */
        seL4_MessageInfo_new(VIBE_OP_PROPOSE, 0, 0, 4)
    );
    
    aos_status_t status = (aos_status_t)seL4_MessageInfo_get_label(reply);
    if (status == AOS_OK) {
        *proposal_id = (uint32_t)seL4_GetMR(0);
    }
    return status;
}

aos_status_t aos_service_proposal_status(uint32_t proposal_id,
                                          char *status_buf, size_t buf_len) {
    seL4_SetMR(0, VIBE_OP_STATUS);
    seL4_SetMR(1, proposal_id);
    seL4_SetMR(2, (uint64_t)(uintptr_t)status_buf);
    seL4_SetMR(3, buf_len);
    
    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_TOOLSVC,
        seL4_MessageInfo_new(VIBE_OP_STATUS, 0, 0, 4)
    );
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_service_swap(uint32_t proposal_id) {
    seL4_MessageInfo_t reply = ipc_call(SLOT_TOOLSVC, VIBE_OP_SWAP, proposal_id, 0, 0);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}
