/*
 * agentOS libagent — Agent SDK Implementation
 *
 * Implements the libagent API declared in agentOS.h.
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
#include <stdarg.h>
#include <stdbool.h>
#include "agentOS.h"
#include "js_runtime.h"
#include "wg_net.h"

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
    bool         initialized;
    agent_id_t   id;
    aos_config_t config;
    char         name[64];
    uint64_t     seqno;
    uint64_t     boot_time_us;
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
    if (!config || !config->name) {
        return AOS_ERR_INVALID;
    }

    memcpy(&agent_state.config, config, sizeof(aos_config_t));
    strncpy(agent_state.name, config->name, sizeof(agent_state.name) - 1);
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
    if (status == 0) {
        /* MsgBus assigned us an id — read it back from MR0..MR3 (32 bytes) */
        for (int i = 0; i < 4; i++) {
            uint64_t word = seL4_GetMR(i);
            memcpy(&agent_state.id.bytes[i * 8], &word, 8);
        }
    }
    /* Non-fatal if MsgBus isn't up yet in early boot — continue anyway */

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

agent_id_t aos_self(void) {
    return agent_state.id;
}

const aos_config_t *aos_config(void) {
    return &agent_state.config;
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
    if (!name) return AOS_CAP_NULL;

    seL4_MessageInfo_t reply = ipc_call_ptr(
        SLOT_MSGBUS, MSGBUS_OP_CREATE,
        name, strlen(name), flags
    );

    /* Returns the channel handle in MR0 */
    return (aos_channel_t)seL4_GetMR(0);
}

aos_channel_t aos_channel_open(const char *name) {
    if (!name) return AOS_CAP_NULL;

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
    msg->msg_type    = (uint32_t)type;
    msg->payload_len = (uint32_t)payload_len;
    msg->schema_ver  = 1;
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

    *output     = out;
    *output_len = (size_t)seL4_GetMR(0);
    return AOS_OK;
}

aos_status_t aos_tool_list(char ***names, size_t *count) {
    if (!names || !count) return AOS_ERR_INVALID;

    seL4_MessageInfo_t reply = ipc_call(SLOT_TOOLSVC, TOOLSVC_OP_LIST, 0, 0, 0);

    *count  = (size_t)seL4_GetMR(0);
    /* Tool names are returned in shared memory — pointer in MR1 */
    *names  = (char **)(uintptr_t)seL4_GetMR(1);

    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

/* =========================================================================
 * Model / Inference API
 * =========================================================================*/

#define MODELSVC_OP_QUERY  0x300
#define MODELSVC_OP_LIST   0x301

aos_inference_t aos_model_query(aos_model_cap_t cap, const char *prompt,
                                 aos_model_params_t *params) {
    aos_inference_t result;
    memset(&result, 0, sizeof(result));
    result.status = AOS_ERR_INVALID;

    if (!prompt) return result;

    /* Pack params into additional message registers before the seL4_Call */
    float    temperature = params ? params->temperature  : 0.7f;
    uint32_t max_tokens  = params ? params->max_tokens   : 2048;
    uint32_t timeout_ms  = params ? params->timeout_ms   : 30000;

    /* Allocate response buffer (up to 64 KB) */
    char *out = (char *)malloc(65536);
    if (!out) {
        result.status = AOS_ERR_NOMEM;
        return result;
    }

    /*
     * Layout: MR[0]=op, MR[1]=cap, MR[2]=prompt ptr, MR[3]=out ptr.
     * A full implementation would also pass temperature/max_tokens via
     * additional MRs or a shared params struct; for now the service uses
     * sensible defaults if those aren't wired yet.
     */
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
        result.response      = out;
        result.tokens_used   = (uint32_t)seL4_GetMR(0);
        result.tokens_prompt = (uint32_t)seL4_GetMR(1);
        result.latency_us    = seL4_GetMR(2);
    } else {
        free(out);
        result.response = NULL;
    }
    return result;
}

void aos_inference_free(aos_inference_t *result) {
    if (result && result->response) {
        free(result->response);
        result->response = NULL;
    }
}

aos_status_t aos_model_list(char ***model_ids, size_t *count) {
    if (!model_ids || !count) return AOS_ERR_INVALID;

    seL4_MessageInfo_t reply = ipc_call(SLOT_MODELSVC, MODELSVC_OP_LIST, 0, 0, 0);
    *count     = (size_t)seL4_GetMR(0);
    *model_ids = (char **)(uintptr_t)seL4_GetMR(1);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

/* =========================================================================
 * Object Store (MemFS)
 * =========================================================================*/

#define MEMFS_OP_OPEN   0x400
#define MEMFS_OP_WRITE  0x401
#define MEMFS_OP_READ   0x402
#define MEMFS_OP_CLOSE  0x403
#define MEMFS_OP_LIST   0x404
#define MEMFS_OP_DELETE 0x405

aos_store_t aos_store_open(cap_t store_cap, const char *path, uint32_t flags) {
    if (!path) return AOS_CAP_NULL;

    seL4_SetMR(0, MEMFS_OP_OPEN);
    seL4_SetMR(1, store_cap);
    seL4_SetMR(2, (uint64_t)(uintptr_t)path);
    seL4_SetMR(3, flags);

    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_MEMFS,
        seL4_MessageInfo_new(MEMFS_OP_OPEN, 0, 0, 4)
    );
    return (aos_store_t)seL4_GetMR(0);
}

ssize_t aos_store_write(aos_store_t h, const void *buf, size_t len) {
    seL4_SetMR(0, MEMFS_OP_WRITE);
    seL4_SetMR(1, (uint64_t)h);
    seL4_SetMR(2, (uint64_t)(uintptr_t)buf);
    seL4_SetMR(3, (uint64_t)len);

    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_MEMFS,
        seL4_MessageInfo_new(MEMFS_OP_WRITE, 0, 0, 4)
    );

    if ((int)seL4_MessageInfo_get_label(reply) != 0) return -1;
    return (ssize_t)seL4_GetMR(0);
}

ssize_t aos_store_read(aos_store_t h, void *buf, size_t len) {
    seL4_SetMR(0, MEMFS_OP_READ);
    seL4_SetMR(1, (uint64_t)h);
    seL4_SetMR(2, (uint64_t)(uintptr_t)buf);
    seL4_SetMR(3, (uint64_t)len);

    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_MEMFS,
        seL4_MessageInfo_new(MEMFS_OP_READ, 0, 0, 4)
    );

    if ((int)seL4_MessageInfo_get_label(reply) != 0) return -1;
    return (ssize_t)seL4_GetMR(0);
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

    *count   = (size_t)seL4_GetMR(0);
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
    seL4_MessageInfo_t reply = ipc_call(SLOT_CAPSTORE, CAPSTORE_OP_DERIVE,
                                        parent, (uint64_t)rights, 0);
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
    seL4_MessageInfo_t reply = ipc_call(SLOT_CAPSTORE, CAPSTORE_OP_REVOKE,
                                        cap, 0, 0);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_cap_list(cap_t **caps, size_t *count) {
    if (!caps || !count) return AOS_ERR_INVALID;
    seL4_MessageInfo_t reply = ipc_call(SLOT_CAPSTORE, CAPSTORE_OP_LIST, 0, 0, 0);
    *count = (size_t)seL4_GetMR(0);
    *caps  = (cap_t *)(uintptr_t)seL4_GetMR(1);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_cap_info(cap_t cap, uint32_t *type, aos_rights_t *rights) {
    seL4_MessageInfo_t reply = ipc_call(SLOT_CAPSTORE, CAPSTORE_OP_INFO,
                                        cap, 0, 0);
    if (type)   *type   = (uint32_t)seL4_GetMR(0);
    if (rights) *rights = (aos_rights_t)seL4_GetMR(1);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

/* =========================================================================
 * Vibe-Coding: Service Proposal & Hot-Swap
 * =========================================================================*/

#define VIBE_OP_PROPOSE     0x700
#define VIBE_OP_STATUS      0x701
#define VIBE_OP_SWAP        0x702
#define VIBE_OP_INFO        0x703

aos_status_t aos_service_propose(const char *service_id,
                                  const void *component_image,
                                  size_t image_len,
                                  uint32_t *proposal_id) {
    if (!service_id || !component_image || !proposal_id) return AOS_ERR_INVALID;

    /* Pass service_id ptr in MR1, image ptr+len in MR2/MR3 */
    seL4_SetMR(0, VIBE_OP_PROPOSE);
    seL4_SetMR(1, (uint64_t)(uintptr_t)service_id);
    seL4_SetMR(2, (uint64_t)(uintptr_t)component_image);
    seL4_SetMR(3, (uint64_t)image_len);

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
                                          uint32_t *validation_state) {
    if (!validation_state) return AOS_ERR_INVALID;

    seL4_MessageInfo_t reply = ipc_call(SLOT_TOOLSVC, VIBE_OP_STATUS,
                                        proposal_id, 0, 0);
    aos_status_t status = (aos_status_t)seL4_MessageInfo_get_label(reply);
    if (status == AOS_OK) {
        *validation_state = (uint32_t)seL4_GetMR(0);
    }
    return status;
}

aos_status_t aos_service_swap(uint32_t proposal_id) {
    seL4_MessageInfo_t reply = ipc_call(SLOT_TOOLSVC, VIBE_OP_SWAP,
                                        proposal_id, 0, 0);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

aos_status_t aos_service_info(const char *service_id,
                               aos_service_iface_t *info) {
    if (!service_id || !info) return AOS_ERR_INVALID;

    seL4_SetMR(0, VIBE_OP_INFO);
    seL4_SetMR(1, (uint64_t)(uintptr_t)service_id);
    seL4_SetMR(2, (uint64_t)(uintptr_t)info);
    seL4_SetMR(3, 0);

    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_TOOLSVC,
        seL4_MessageInfo_new(VIBE_OP_INFO, 0, 0, 4)
    );
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

/* =========================================================================
 * NetStack / HTTP client — bridge communication
 * =========================================================================*/

/*
 * SLOT_NETSTK: CSpace slot 8 is the NetStack service endpoint.
 * The init task grants this capability at agent spawn time, parallel to the
 * other service endpoints.  NetStack proxies HTTP requests to the agentOS
 * bridge running on the host at 10.0.2.2:8790.
 */
#define SLOT_NETSTK           8    /* NetStack service endpoint */
#define SLOT_JS_RUNTIME       9    /* JS/QuickJS runtime service endpoint */
#define SLOT_WG_NET           10   /* WireGuard network service endpoint */
#define NETSTK_OP_HTTP_POST   0x500

/*
 * We share the vibe_staging region (set up by the init task) for HTTP I/O.
 * The staging region is 4MB.  The first 2MB (offset 0x000000..0x1FFFFF) is
 * the primary WASM staging area used by VibeEngine.  We use the second 2MB
 * (offset 0x200000..0x3FFFFF) for HTTP request/response payloads so the two
 * uses never collide.
 *
 * Within the compile path we additionally need 1MB for the JSON request body
 * (offset 0x100000..0x1FFFFF in the WASM half) — that's still inside the
 * upper 2MB window, so:
 *
 *   0x000000 – 0x0FFFFF  : WASM binary output (primary staging)
 *   0x100000 – 0x1FFFFF  : compile JSON request body (sub-region)
 *   0x200000 – 0x3FFFFF  : HTTP generic request/response
 */
extern uintptr_t vibe_staging_vaddr;

/* URL bounce buffer: 4KB slot immediately before the HTTP body region.
 * net_server reads the URL from staging (cross-PD dereference via shared
 * memory) rather than using the agent's raw VA pointer, which would fault. */
#define STAGING_HTTP_URL_OFFSET  0x1FF000UL  /* 4 KB before STAGING_HTTP_OFFSET */
#define STAGING_HTTP_URL_MAX     4096UL

#define STAGING_HTTP_OFFSET    0x200000UL   /* second 2MB: generic HTTP I/O  */
#define STAGING_HTTP_MAX       (2UL * 1024UL * 1024UL)

/* Sub-region for compile request JSON (first 1MB of the second 2MB block) */
#define STAGING_COMPILE_REQ_OFFSET  0x100000UL
#define STAGING_COMPILE_REQ_MAX     (1UL * 1024UL * 1024UL)

/* -------------------------------------------------------------------------
 * Minimal base64 decoder
 * Standard alphabet (A-Z a-z 0-9 + /), = padding, no line wrapping.
 * Returns number of bytes written to out, or -1 on error.
 * ------------------------------------------------------------------------- */
static int b64_decode(const char *in, size_t in_len,
                      uint8_t *out, size_t out_max)
{
    static const int8_t dtab[256] = {
        /* 0x00-0x2B */ -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        /* 0x2B '+' */ 62,
        /* 0x2C ',' */ -1,
        /* 0x2D '-' */ -1,
        /* 0x2E '.' */ -1,
        /* 0x2F '/' */ 63,
        /* 0x30-0x39 '0'-'9' */ 52,53,54,55,56,57,58,59,60,61,
        /* 0x3A-0x40 */ -1,-1,-1,-1,-1,-1,-1,
        /* 0x41-0x5A 'A'-'Z' */ 0,1,2,3,4,5,6,7,8,9,10,11,12,
                                13,14,15,16,17,18,19,20,21,22,23,24,25,
        /* 0x5B-0x60 */ -1,-1,-1,-1,-1,-1,
        /* 0x61-0x7A 'a'-'z' */ 26,27,28,29,30,31,32,33,34,35,36,37,38,
                                39,40,41,42,43,44,45,46,47,48,49,50,51,
        /* 0x7B-0xFF */ -1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    size_t out_pos = 0;
    uint32_t acc   = 0;
    int      bits  = 0;

    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=') break;               /* padding — done */
        int v = dtab[c];
        if (v < 0) continue;              /* skip unknown chars (whitespace) */

        acc  = (acc << 6) | (uint32_t)v;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            if (out_pos >= out_max) return -1; /* output overflow */
            out[out_pos++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }

    return (int)out_pos;
}

/* -------------------------------------------------------------------------
 * JSON string extractor
 *
 * Finds the first occurrence of `key` (e.g., `"code"`) in `json`, then
 * extracts the string value that follows (handles \" escapes, no Unicode
 * escape expansion needed for our use case).
 *
 * Returns pointer to the start of the string value in json (after the
 * opening quote), and sets *val_len to the decoded length.
 * Returns NULL if the key is not found or the value is malformed.
 * ------------------------------------------------------------------------- */
static const char *json_extract_string(const char *json, const char *key,
                                        size_t *val_len)
{
    /* Build the search pattern: "key": (or "key" :) */
    /* We search for `"key"` then skip whitespace and `:` then opening `"` */
    size_t klen = strlen(key);
    const char *p = json;

    while (*p) {
        /* Find next occurrence of `"key"` */
        if (*p != '"') { p++; continue; }
        /* Check if this is our key */
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            p += 1 + klen + 1; /* skip past closing quote of key */
            /* Skip whitespace */
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p != ':') { continue; } /* not a key:value pair */
            p++;
            /* Skip whitespace after colon */
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p != '"') return NULL; /* value isn't a string */
            p++; /* skip opening quote */
            /* p now points at the first char of the string value */
            const char *start = p;
            size_t      len   = 0;
            while (*p && *p != '"') {
                if (*p == '\\') {
                    p++; /* skip escape char */
                    if (!*p) break;
                }
                p++;
                len++;
            }
            *val_len = len;
            return start;
        }
        p++;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * json_unescape_string — copy a JSON string value with escape processing.
 *
 * Reads from `src` (the raw JSON string value, after the opening quote,
 * as returned by json_extract_string) for `src_len` characters and writes
 * the unescaped result to `dst` (which must hold at least `dst_max` bytes).
 * The dst is null-terminated.  Returns the number of bytes written (excl NUL).
 * ------------------------------------------------------------------------- */
static size_t json_unescape_string(const char *src, size_t src_len,
                                   char *dst, size_t dst_max)
{
    size_t di = 0;
    for (size_t si = 0; si < src_len && di + 1 < dst_max; si++) {
        if (src[si] == '\\' && si + 1 < src_len) {
            si++;
            switch (src[si]) {
            case '"':  dst[di++] = '"';  break;
            case '\\': dst[di++] = '\\'; break;
            case '/':  dst[di++] = '/';  break;
            case 'n':  dst[di++] = '\n'; break;
            case 'r':  dst[di++] = '\r'; break;
            case 't':  dst[di++] = '\t'; break;
            default:   dst[di++] = src[si]; break;
            }
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
    return di;
}

/* -------------------------------------------------------------------------
 * aos_http_post — make an HTTP POST to the agentOS bridge.
 *
 * Uses the vibe staging region (STAGING_HTTP_OFFSET into the shared 4MB
 * region) as a bounce buffer for the request and response bodies, then
 * PPCs to the NetStack PD (SLOT_NETSTK) which forwards the call to the
 * bridge at 10.0.2.2:8790.
 * ------------------------------------------------------------------------- */
aos_status_t aos_http_post(const char *url, const char *body_json,
                            char *resp_buf, size_t resp_buf_len,
                            size_t *resp_len)
{
    if (!url || !body_json || !resp_buf || !resp_len)
        return AOS_ERR_INVALID;

    /* Staging region must be mapped before we can use it */
    if (vibe_staging_vaddr == 0)
        return AOS_ERR_IO;

    size_t body_len = strlen(body_json);
    if (body_len >= STAGING_HTTP_MAX)
        return AOS_ERR_INVALID; /* request too large */

    size_t url_len = strlen(url);
    if (url_len >= STAGING_HTTP_URL_MAX)
        return AOS_ERR_INVALID; /* URL too long */

    /* Copy URL into staging so net_server can read it cross-PD */
    char *staging_url = (char *)(vibe_staging_vaddr + STAGING_HTTP_URL_OFFSET);
    memcpy(staging_url, url, url_len + 1); /* include NUL */

    /* Copy request body into staging at STAGING_HTTP_OFFSET */
    char *staging_req = (char *)(vibe_staging_vaddr + STAGING_HTTP_OFFSET);
    memcpy(staging_req, body_json, body_len + 1); /* include NUL */

    /* PPC to NetStack (MR0 carries opcode redundantly; label field is canonical):
     *   MR1 = STAGING_HTTP_URL_OFFSET  (URL location in staging — not a raw VA)
     *   MR2 = url_len
     *   MR3 = STAGING_HTTP_OFFSET      (request body location in staging)
     *   MR4 = body_len
     */
    seL4_SetMR(0, (seL4_Word)NETSTK_OP_HTTP_POST);
    seL4_SetMR(1, (seL4_Word)STAGING_HTTP_URL_OFFSET);
    seL4_SetMR(2, (seL4_Word)url_len);
    seL4_SetMR(3, (seL4_Word)STAGING_HTTP_OFFSET);
    seL4_SetMR(4, (seL4_Word)body_len);

    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_NETSTK,
        seL4_MessageInfo_new(NETSTK_OP_HTTP_POST, 0, 0, 5)
    );
    (void)reply;

    /* Reply layout:
     *   MR0 = HTTP status code (200/201 = ok, 0 = NetStack not running)
     *   MR1 = response body offset in staging
     *   MR2 = response body length
     */
    uint64_t http_status   = seL4_GetMR(0);
    uint64_t resp_offset   = seL4_GetMR(1);
    uint64_t resp_body_len = seL4_GetMR(2);

    if (http_status == 0) {
        /* NetStack is not yet running (stub path) */
        *resp_len = 0;
        return AOS_ERR_IO;
    }

    if (http_status != 200 && http_status != 201) {
        *resp_len = 0;
        return AOS_ERR_IO;
    }

    /* Copy response from staging into caller's buffer */
    if (resp_body_len >= resp_buf_len) {
        /* Truncate to fit — still return AOS_OK but signal via resp_len */
        resp_body_len = resp_buf_len - 1;
    }

    const char *staging_resp = (const char *)(vibe_staging_vaddr + resp_offset);
    memcpy(resp_buf, staging_resp, (size_t)resp_body_len);
    resp_buf[resp_body_len] = '\0';
    *resp_len = (size_t)resp_body_len;

    return AOS_OK;
}

/* -------------------------------------------------------------------------
 * aos_vibe_generate — request code generation from the bridge.
 *
 * Builds a JSON body, POSTs to /api/agentos/vibe/generate, and parses
 * the "code" field from the JSON response.
 * ------------------------------------------------------------------------- */
aos_status_t aos_vibe_generate(const char *prompt, const char *service_id,
                                char *out_code, size_t out_code_len,
                                size_t *actual_len)
{
    if (!prompt || !out_code || !actual_len)
        return AOS_ERR_INVALID;

    /* Build JSON request body on the stack.
     * 4KB is generous for a prompt string. */
    char body[4096];
    int  n;
    if (service_id && service_id[0]) {
        n = snprintf(body, sizeof(body),
                     "{\"prompt\": \"%s\", \"service_id\": \"%s\"}",
                     prompt, service_id);
    } else {
        n = snprintf(body, sizeof(body),
                     "{\"prompt\": \"%s\"}",
                     prompt);
    }
    if (n < 0 || (size_t)n >= sizeof(body))
        return AOS_ERR_INVALID;

    /* Use a single large response buffer — up to 128KB JSON response */
    static char resp[131072]; /* 128 KB — holds generated C with JSON framing */
    size_t resp_len = 0;

    aos_status_t err = aos_http_post(
        "http://10.0.2.2:8790/api/agentos/vibe/generate",
        body, resp, sizeof(resp), &resp_len);
    if (err != AOS_OK)
        return err;

    /*
     * Parse the "code" key from the JSON response.
     * json_extract_string returns a pointer into resp and the raw (escaped)
     * length; we then unescape it into out_code.
     */
    size_t raw_len = 0;
    const char *raw = json_extract_string(resp, "code", &raw_len);
    if (!raw || raw_len == 0)
        return AOS_ERR_PROTO;

    size_t written = json_unescape_string(raw, raw_len, out_code, out_code_len);
    *actual_len = written;
    return AOS_OK;
}

/* -------------------------------------------------------------------------
 * aos_vibe_compile — compile C source to WASM via the bridge.
 *
 * The compiled WASM binary is written into the vibe staging region at
 * offset 0, ready for OP_VIBE_PROPOSE.
 * ------------------------------------------------------------------------- */
aos_status_t aos_vibe_compile(const char *source_c, const char *service_id,
                               uint32_t *wasm_size_out)
{
    if (!source_c || !wasm_size_out)
        return AOS_ERR_INVALID;

    if (vibe_staging_vaddr == 0)
        return AOS_ERR_IO;

    /*
     * Build the JSON request body.  We JSON-escape the C source inline into
     * the staging sub-region at STAGING_COMPILE_REQ_OFFSET (1MB window).
     * Using staging avoids a large stack allocation for potentially huge
     * source files.
     */
    char *req_buf = (char *)(vibe_staging_vaddr + STAGING_COMPILE_REQ_OFFSET);
    size_t req_max = STAGING_COMPILE_REQ_MAX;

    /* Write the JSON prefix */
    size_t pos = 0;
    const char *prefix_fmt = "{\"source_c\": \"";
    size_t prefix_len = strlen(prefix_fmt);
    if (prefix_len >= req_max) return AOS_ERR_NOMEM;
    memcpy(req_buf + pos, prefix_fmt, prefix_len);
    pos += prefix_len;

    /* JSON-escape the C source */
    for (const char *p = source_c; *p && pos + 4 < req_max; p++) {
        switch (*p) {
        case '\\': req_buf[pos++] = '\\'; req_buf[pos++] = '\\'; break;
        case '"':  req_buf[pos++] = '\\'; req_buf[pos++] = '"';  break;
        case '\n': req_buf[pos++] = '\\'; req_buf[pos++] = 'n';  break;
        case '\r': req_buf[pos++] = '\\'; req_buf[pos++] = 'r';  break;
        case '\t': req_buf[pos++] = '\\'; req_buf[pos++] = 't';  break;
        default:   req_buf[pos++] = *p;                          break;
        }
    }

    /* Write the JSON suffix with service_id */
    char suffix[256];
    int sn;
    if (service_id && service_id[0]) {
        sn = snprintf(suffix, sizeof(suffix),
                      "\", \"service_id\": \"%s\"}", service_id);
    } else {
        sn = snprintf(suffix, sizeof(suffix), "\"}");
    }
    if (sn < 0 || (size_t)sn >= sizeof(suffix)) return AOS_ERR_INVALID;
    if (pos + (size_t)sn >= req_max) return AOS_ERR_NOMEM;
    memcpy(req_buf + pos, suffix, (size_t)sn + 1); /* include NUL */
    pos += (size_t)sn;

    /*
     * POST to the bridge compile endpoint.
     * The response JSON contains a "wasm_b64" field with the base64-encoded
     * WASM binary.  We parse it and decode directly into the staging region
     * at offset 0 (the primary WASM area), ready for OP_VIBE_PROPOSE.
     *
     * The response can be large (base64 of a WASM binary).  We decode from
     * the HTTP response that NetStack places in the staging HTTP window
     * (offset STAGING_HTTP_OFFSET) directly, then base64-decode into offset 0.
     *
     * For the response buffer we point at the HTTP staging window — it's
     * already in shared memory.  Pass it as a local pointer.
     */
    static char resp[131072]; /* 128KB for the JSON wrapper + b64 data */
    size_t resp_len = 0;

    aos_status_t err = aos_http_post(
        "http://10.0.2.2:8790/api/agentos/vibe/compile",
        req_buf, resp, sizeof(resp), &resp_len);
    if (err != AOS_OK)
        return err;

    /* Parse "wasm_b64" from response */
    size_t b64_raw_len = 0;
    const char *b64_raw = json_extract_string(resp, "wasm_b64", &b64_raw_len);
    if (!b64_raw || b64_raw_len == 0)
        return AOS_ERR_PROTO;

    /*
     * Decode directly into the primary WASM staging region at offset 0.
     * The WASM staging area is 1MB (0x000000..0x0FFFFF).
     * The compile request sits in offset 0x100000..0x1FFFFF, so no overlap.
     */
    uint8_t *wasm_dst = (uint8_t *)vibe_staging_vaddr; /* offset 0 */
    size_t   wasm_max = STAGING_COMPILE_REQ_OFFSET;    /* 1MB */

    int decoded = b64_decode(b64_raw, b64_raw_len, wasm_dst, wasm_max);
    if (decoded < 0)
        return AOS_ERR_PROTO;

    *wasm_size_out = (uint32_t)decoded;
    return AOS_OK;
}

/* =========================================================================
 * JS Runtime — QuickJS evaluation and function calls
 * =========================================================================*/

/*
 * JS staging region: a separate 4MB shared region (js_staging) mapped at
 * js_staging_vaddr.  Sub-region layout mirrors js_runtime.h:
 *   [0x000000 .. 0x1FFFFF]  script / module source input (2MB)
 *   [0x200000 .. 0x3FFFFF]  evaluation output / return values (2MB)
 *
 * For OP_JS_CALL, function name and args share the input region:
 *   func_name at offset 0x000000
 *   args JSON  at offset 0x001000 (4KB past func_name)
 */
extern uintptr_t js_staging_vaddr;

#define JS_STAGING_BASE         0x400000UL   /* total JS staging size (4MB) */
#define JS_INPUT_OFFSET         0x000000UL   /* script/func-name input base */
#define JS_OUTPUT_OFFSET        0x200000UL   /* evaluation/call output base */
#define JS_ARGS_OFFSET          0x001000UL   /* args JSON: 4KB into input region */

/* -------------------------------------------------------------------------
 * aos_js_eval — evaluate a JavaScript string in a QuickJS context.
 *
 * Writes the script source into js_staging at JS_INPUT_OFFSET, then sends
 * OP_JS_EVAL to SLOT_JS_RUNTIME.  The result text (JSON-stringified) is
 * copied from js_staging at the output_offset reported in the reply.
 * ------------------------------------------------------------------------- */
aos_status_t aos_js_eval(uint32_t context_id,
                          const char *script,
                          char *out_buf, size_t out_buf_len,
                          size_t *out_len)
{
    if (!script || !out_buf || !out_len)
        return AOS_ERR_INVALID;

    if (js_staging_vaddr == 0)
        return AOS_ERR_IO;

    size_t script_len = strlen(script);
    if (script_len >= JS_STAGING_BASE / 2)
        return AOS_ERR_INVALID; /* script too large for input region */

    /* Copy script into js_staging at the input offset */
    char *staging_input = (char *)(js_staging_vaddr + JS_INPUT_OFFSET);
    memcpy(staging_input, script, script_len); /* no NUL required by protocol */

    /*
     * PPC to JS runtime:
     *   MR0 = OP_JS_EVAL  (label field carries opcode redundantly)
     *   MR1 = context_id  (0xFF = auto-create)
     *   MR2 = script_offset (JS_INPUT_OFFSET into js_staging)
     *   MR3 = script_len
     */
    seL4_SetMR(0, (seL4_Word)OP_JS_EVAL);
    seL4_SetMR(1, (seL4_Word)context_id);
    seL4_SetMR(2, (seL4_Word)JS_INPUT_OFFSET);
    seL4_SetMR(3, (seL4_Word)script_len);

    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_JS_RUNTIME,
        seL4_MessageInfo_new(OP_JS_EVAL, 0, 0, 4)
    );

    /*
     * Reply layout:
     *   MR0 = result  (JS_OK or JS_ERR_*)
     *   MR1 = context_id  (assigned or echoed)
     *   MR2 = output_offset (byte offset into js_staging)
     *   MR3 = output_len
     */
    uint64_t js_result     = seL4_GetMR(0);
    /* MR1 (context_id echo) intentionally not stored — caller can re-use input */
    uint64_t output_offset = seL4_GetMR(2);
    uint64_t output_len    = seL4_GetMR(3);
    (void)reply;

    if (js_result != JS_OK) {
        /* On error MR2/MR3 describe the error string in staging — copy it too */
        if (output_len > 0 && out_buf_len > 0) {
            size_t copy = (output_len < out_buf_len - 1)
                          ? (size_t)output_len : out_buf_len - 1;
            memcpy(out_buf, (char *)(js_staging_vaddr + output_offset), copy);
            out_buf[copy] = '\0';
            *out_len = copy;
        } else {
            *out_len = 0;
        }
        return AOS_ERR_IO;
    }

    /* Copy output from staging into caller's buffer */
    if (output_len >= out_buf_len)
        output_len = out_buf_len - 1; /* truncate to fit */

    memcpy(out_buf, (char *)(js_staging_vaddr + output_offset),
           (size_t)output_len);
    out_buf[output_len] = '\0';
    *out_len = (size_t)output_len;

    return AOS_OK;
}

/* -------------------------------------------------------------------------
 * aos_js_call — call a named JavaScript function in an existing context.
 *
 * Writes func_name at JS_INPUT_OFFSET and args_json at JS_ARGS_OFFSET
 * within js_staging, then sends OP_JS_CALL to SLOT_JS_RUNTIME.
 * ------------------------------------------------------------------------- */
aos_status_t aos_js_call(uint32_t context_id,
                          const char *func_name,
                          const char *args_json,
                          char *out_buf, size_t out_buf_len,
                          size_t *out_len)
{
    if (!func_name || !out_buf || !out_len)
        return AOS_ERR_INVALID;

    if (js_staging_vaddr == 0)
        return AOS_ERR_IO;

    size_t fname_len = strlen(func_name);
    size_t args_len  = args_json ? strlen(args_json) : 0;

    /* func_name must fit in the 4KB slot before JS_ARGS_OFFSET */
    if (fname_len >= JS_ARGS_OFFSET)
        return AOS_ERR_INVALID;

    /* Copy func_name into staging at input offset */
    char *staging_fname = (char *)(js_staging_vaddr + JS_INPUT_OFFSET);
    memcpy(staging_fname, func_name, fname_len);

    /* Copy args JSON into staging at args offset (within input region) */
    if (args_len > 0) {
        char *staging_args = (char *)(js_staging_vaddr + JS_INPUT_OFFSET
                                      + JS_ARGS_OFFSET);
        memcpy(staging_args, args_json, args_len);
    }

    /*
     * PPC to JS runtime:
     *   MR0 = OP_JS_CALL
     *   MR1 = context_id
     *   MR2 = func_name_offset (JS_INPUT_OFFSET into js_staging)
     *   MR3 = func_name_len
     *   MR4 = args_offset      (JS_INPUT_OFFSET + JS_ARGS_OFFSET)
     *   MR5 = args_len         (0 = no args / empty array)
     */
    seL4_SetMR(0, (seL4_Word)OP_JS_CALL);
    seL4_SetMR(1, (seL4_Word)context_id);
    seL4_SetMR(2, (seL4_Word)JS_INPUT_OFFSET);
    seL4_SetMR(3, (seL4_Word)fname_len);
    seL4_SetMR(4, (seL4_Word)(JS_INPUT_OFFSET + JS_ARGS_OFFSET));
    seL4_SetMR(5, (seL4_Word)args_len);

    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_JS_RUNTIME,
        seL4_MessageInfo_new(OP_JS_CALL, 0, 0, 6)
    );

    /*
     * Reply layout:
     *   MR0 = result (JS_OK or JS_ERR_*)
     *   MR1 = output_offset (byte offset into js_staging)
     *   MR2 = output_len
     */
    uint64_t js_result     = seL4_GetMR(0);
    uint64_t output_offset = seL4_GetMR(1);
    uint64_t output_len    = seL4_GetMR(2);
    (void)reply;

    if (js_result != JS_OK) {
        if (output_len > 0 && out_buf_len > 0) {
            size_t copy = (output_len < out_buf_len - 1)
                          ? (size_t)output_len : out_buf_len - 1;
            memcpy(out_buf, (char *)(js_staging_vaddr + output_offset), copy);
            out_buf[copy] = '\0';
            *out_len = copy;
        } else {
            *out_len = 0;
        }
        return AOS_ERR_IO;
    }

    if (output_len >= out_buf_len)
        output_len = out_buf_len - 1;

    memcpy(out_buf, (char *)(js_staging_vaddr + output_offset),
           (size_t)output_len);
    out_buf[output_len] = '\0';
    *out_len = (size_t)output_len;

    return AOS_OK;
}

/* =========================================================================
 * WireGuard Overlay Network
 * =========================================================================*/

/*
 * WireGuard staging region (wg_staging) is a separate shared region used
 * for key material and packet buffers.  We share the existing vibe_staging
 * region for the WG key/peer scratch and packet buffers, using offsets that
 * do not collide with any existing sub-region:
 *
 *   WG scratch (pubkey for OP_WG_ADD_PEER): 4KB slot at 0x1FE000
 *     = STAGING_HTTP_URL_OFFSET - 0x1000
 *   WG TX data (OP_WG_SEND): 1MB into the HTTP region
 *     = STAGING_HTTP_OFFSET + 0x100000
 *
 * OP_WG_RECV reply offsets are provided by wg_net itself (into its own
 * wg_staging RX region); the caller copies from that offset.
 *
 * Note: The wg_net PD has its own wg_staging mapping.  The offsets used in
 * MR2/MR3 below refer to *wg_net's* staging VA, not to vibe_staging.  By
 * convention wg_net's pubkey staging begins at wg_staging[0x001000] and its
 * TX region at wg_staging[0x002000].  We place data at those same offsets
 * inside vibe_staging and pass the offsets to wg_net, which resolves them
 * against its own mapping.
 */

/* Offsets into vibe_staging used as WG scratch areas */
#define STAGING_WG_PUBKEY_OFFSET   0x1FE000UL  /* 4KB WG pubkey scratch */
#define STAGING_WG_TX_OFFSET       (STAGING_HTTP_OFFSET + 0x100000UL)

/* Corresponding offsets from wg_net's perspective (wg_staging layout) */
#define WG_STAGING_PUBKEY_OFFSET   0x001000UL  /* peer pubkey staging in wg_net */
#define WG_STAGING_TX_OFFSET       0x002000UL  /* TX staging in wg_net */

/* -------------------------------------------------------------------------
 * aos_wg_add_peer — register a WireGuard peer with the wg_net PD.
 *
 * Copies the 32-byte public key into vibe_staging then PPCs to SLOT_WG_NET
 * with OP_WG_ADD_PEER.  The pubkey_staging_offset sent in MR2 must match
 * the offset at which wg_net expects to find the key in *its* wg_staging.
 * ------------------------------------------------------------------------- */
aos_status_t aos_wg_add_peer(uint8_t peer_id,
                              const uint8_t pubkey[32],
                              uint32_t endpoint_ip_be,
                              uint16_t endpoint_port,
                              uint32_t allowed_ip_be,
                              uint32_t allowed_mask)
{
    if (!pubkey)
        return AOS_ERR_INVALID;

    if (vibe_staging_vaddr == 0)
        return AOS_ERR_IO;

    /* Write the 32-byte public key into the WG scratch slot in staging */
    uint8_t *staging_pubkey = (uint8_t *)(vibe_staging_vaddr
                                          + STAGING_WG_PUBKEY_OFFSET);
    memcpy(staging_pubkey, pubkey, 32);

    /*
     * PPC to wg_net:
     *   MR0 = OP_WG_ADD_PEER
     *   MR1 = peer_id
     *   MR2 = pubkey_staging_offset  (wg_net's staging offset for the key)
     *   MR3 = endpoint_ip_be
     *   MR4 = endpoint_port
     *   MR5 = allowed_ip_be
     *   MR6 = allowed_mask
     */
    seL4_SetMR(0, (seL4_Word)OP_WG_ADD_PEER);
    seL4_SetMR(1, (seL4_Word)peer_id);
    seL4_SetMR(2, (seL4_Word)WG_STAGING_PUBKEY_OFFSET);
    seL4_SetMR(3, (seL4_Word)endpoint_ip_be);
    seL4_SetMR(4, (seL4_Word)endpoint_port);
    seL4_SetMR(5, (seL4_Word)allowed_ip_be);
    seL4_SetMR(6, (seL4_Word)allowed_mask);

    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_WG_NET,
        seL4_MessageInfo_new(OP_WG_ADD_PEER, 0, 0, 7)
    );

    uint64_t wg_result = seL4_GetMR(0);
    (void)reply;

    if (wg_result != WG_OK)
        return AOS_ERR_IO;

    return AOS_OK;
}

/* -------------------------------------------------------------------------
 * aos_wg_send — encrypt and transmit data to a WireGuard peer.
 *
 * Copies plaintext into vibe_staging at STAGING_WG_TX_OFFSET (which maps
 * to wg_net's TX staging at WG_STAGING_TX_OFFSET), then PPCs OP_WG_SEND.
 * ------------------------------------------------------------------------- */
aos_status_t aos_wg_send(uint8_t peer_id,
                          const uint8_t *data, uint32_t len)
{
    if (!data || len == 0)
        return AOS_ERR_INVALID;

    if (vibe_staging_vaddr == 0)
        return AOS_ERR_IO;

    /* Sanity check: WG max payload is 65535 bytes */
    if (len > 65535)
        return AOS_ERR_INVALID;

    /* Copy plaintext into the TX scratch region in vibe_staging */
    uint8_t *staging_tx = (uint8_t *)(vibe_staging_vaddr + STAGING_WG_TX_OFFSET);
    memcpy(staging_tx, data, len);

    /*
     * PPC to wg_net:
     *   MR0 = OP_WG_SEND
     *   MR1 = peer_id
     *   MR2 = data_offset  (TX staging offset in wg_net's mapping)
     *   MR3 = len
     */
    seL4_SetMR(0, (seL4_Word)OP_WG_SEND);
    seL4_SetMR(1, (seL4_Word)peer_id);
    seL4_SetMR(2, (seL4_Word)WG_STAGING_TX_OFFSET);
    seL4_SetMR(3, (seL4_Word)len);

    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_WG_NET,
        seL4_MessageInfo_new(OP_WG_SEND, 0, 0, 4)
    );

    uint64_t wg_result = seL4_GetMR(0);
    (void)reply;

    if (wg_result != WG_OK)
        return AOS_ERR_IO;

    return AOS_OK;
}

/* -------------------------------------------------------------------------
 * aos_wg_recv — poll for a decrypted packet from a WireGuard peer.
 *
 * PPCs OP_WG_RECV to wg_net.  On WG_OK the decrypted payload sits in
 * wg_net's RX staging at data_offset for data_len bytes; wg_net resolves
 * that offset against its own mapping and writes the data there.  Because
 * wg_staging is shared with the agent, we read directly from that offset
 * via our own mapping (js_staging_vaddr is reused here — if the kernel
 * maps wg_staging at a separate VA, replace with the correct extern).
 *
 * Non-blocking: returns AOS_OK with *recv_len=0 if no packet is pending.
 * ------------------------------------------------------------------------- */
extern uintptr_t wg_staging_vaddr;

aos_status_t aos_wg_recv(uint8_t peer_id,
                          uint8_t *buf, uint32_t max_len,
                          uint32_t *recv_len)
{
    if (!buf || !recv_len)
        return AOS_ERR_INVALID;

    if (wg_staging_vaddr == 0)
        return AOS_ERR_IO;

    /*
     * PPC to wg_net:
     *   MR0 = OP_WG_RECV
     *   MR1 = peer_id   (0xFF = receive from any peer)
     *   MR2 = out_offset (RX staging offset in wg_net — wg_net writes here)
     *   MR3 = max_len
     *
     * wg_net ignores MR2/MR3 in current design and returns the offset/len
     * where it placed the data.  We pass WG_STAGING_RX_OFFSET as a hint.
     */
#define WG_STAGING_RX_OFFSET  0x010000UL  /* RX staging offset per wg_net.h */

    seL4_SetMR(0, (seL4_Word)OP_WG_RECV);
    seL4_SetMR(1, (seL4_Word)peer_id);
    seL4_SetMR(2, (seL4_Word)WG_STAGING_RX_OFFSET);
    seL4_SetMR(3, (seL4_Word)max_len);

    seL4_MessageInfo_t reply = seL4_Call(
        SLOT_WG_NET,
        seL4_MessageInfo_new(OP_WG_RECV, 0, 0, 4)
    );

    /*
     * Reply layout (from wg_net.h OP_WG_RECV):
     *   MR0 = result
     *   MR1 = src_peer_id
     *   MR2 = data_offset (byte offset into wg_staging RX region)
     *   MR3 = data_len    (0 if no packet pending)
     */
    uint64_t wg_result   = seL4_GetMR(0);
    /* MR1 src_peer_id — not surfaced in this API but available to extend */
    uint64_t data_offset = seL4_GetMR(2);
    uint64_t data_len    = seL4_GetMR(3);
    (void)reply;

    if (wg_result != WG_OK) {
        *recv_len = 0;
        return AOS_ERR_IO;
    }

    if (data_len == 0) {
        *recv_len = 0;
        return AOS_OK; /* non-blocking: no packet pending */
    }

    /* Truncate to caller's buffer */
    if (data_len > max_len)
        data_len = max_len;

    /* Copy decrypted payload from wg_staging into caller's buffer */
    memcpy(buf, (uint8_t *)(wg_staging_vaddr + data_offset), (size_t)data_len);
    *recv_len = (uint32_t)data_len;

    return AOS_OK;
}

/* =========================================================================
 * Utility
 * =========================================================================*/

#define LOGSVC_OP_LOG       0x600
#define LOGSVC_OP_TIME_US   0x601
#define LOGSVC_OP_SLEEP_US  0x602

uint64_t aos_time_us(void) {
    seL4_MessageInfo_t reply = ipc_call(SLOT_LOGSVC, LOGSVC_OP_TIME_US,
                                        0, 0, 0);
    if (seL4_MessageInfo_get_label(reply) != 0) return 0;
    /* Two 32-bit halves packed into MR0/MR1 for 64-bit time */
    uint64_t hi = seL4_GetMR(1);
    uint64_t lo = seL4_GetMR(0);
    return (hi << 32) | lo;
}

aos_status_t aos_sleep_us(uint64_t us) {
    seL4_MessageInfo_t reply = ipc_call(SLOT_LOGSVC, LOGSVC_OP_SLEEP_US,
                                        (uint64_t)(us & 0xFFFFFFFF),
                                        (uint64_t)(us >> 32),
                                        0);
    return (aos_status_t)seL4_MessageInfo_get_label(reply);
}

/*
 * Log a message.  Formats into a stack buffer then sends pointer+len
 * to LogSvc via IPC.  On seL4 the kernel's debug_put_char is available
 * if LogSvc hasn't started yet (early-boot fallback handled by service).
 */
void aos_log(uint32_t level, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);

    seL4_SetMR(0, LOGSVC_OP_LOG);
    seL4_SetMR(1, level);
    seL4_SetMR(2, (uint64_t)(uintptr_t)buf);
    seL4_SetMR(3, (uint64_t)n);

    /* Fire-and-forget: we don't wait for the reply to avoid blocking the
     * caller's hot path.  Use seL4_Send (no reply cap needed). */
    seL4_Send(SLOT_LOGSVC, seL4_MessageInfo_new(LOGSVC_OP_LOG, 0, 0, 4));
}
