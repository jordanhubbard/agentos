/*
 * agentOS SDK — libagent
 * 
 * The primary interface for agents running on agentOS.
 * Built on seL4 capabilities and IPC primitives.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef AGENTOS_H
#define AGENTOS_H

#include <stdint.h>
#include <stddef.h>

/* ssize_t — signed size type. Defined here for freestanding/seL4 builds
 * where <sys/types.h> may not be available. */
#ifndef __ssize_t_defined
typedef long ssize_t;
#define __ssize_t_defined
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * =============================================================================
 * Types
 * =============================================================================
 */

/* Agent identity — Ed25519 public key hash */
typedef struct {
    uint8_t bytes[32];
} agent_id_t;

/* Capability handle (wraps seL4_CPtr) */
typedef uint64_t cap_t;

/* Null capability */
#define AOS_CAP_NULL ((cap_t)0)

/* Status codes */
typedef enum {
    AOS_OK              = 0,
    AOS_ERR_NOCAP       = -1,   /* Missing required capability */
    AOS_ERR_DENIED      = -2,   /* Capability insufficient for operation */
    AOS_ERR_NOMEM       = -3,   /* Out of memory */
    AOS_ERR_TIMEOUT     = -4,   /* Operation timed out */
    AOS_ERR_INVALID     = -5,   /* Invalid argument */
    AOS_ERR_NOTFOUND    = -6,   /* Resource not found */
    AOS_ERR_EXISTS      = -7,   /* Resource already exists */
    AOS_ERR_BUSY        = -8,   /* Resource busy / in use */
    AOS_ERR_IO          = -9,   /* I/O error */
    AOS_ERR_PROTO       = -10,  /* Protocol error */
    AOS_ERR_INTERNAL    = -99,  /* Internal error */
} aos_status_t;

/* Message types */
typedef enum {
    AOS_MSG_TEXT        = 0x01,  /* Structured text (UTF-8, schema-tagged) */
    AOS_MSG_BLOB        = 0x02,  /* Binary blob */
    AOS_MSG_CAP         = 0x03,  /* Capability transfer */
    AOS_MSG_EVENT       = 0x04,  /* Event notification */
    AOS_MSG_RPC_REQ     = 0x05,  /* RPC request */
    AOS_MSG_RPC_REPLY   = 0x06,  /* RPC reply */
    AOS_MSG_HEARTBEAT   = 0x07,  /* Heartbeat / presence */
    AOS_MSG_TOOL_CALL   = 0x10,  /* Tool invocation */
    AOS_MSG_TOOL_RESULT = 0x11,  /* Tool result */
    AOS_MSG_MODEL_REQ   = 0x20,  /* Model inference request */
    AOS_MSG_MODEL_RESP  = 0x21,  /* Model inference response */
} aos_msg_type_t;

/* Capability rights bitfield */
typedef enum {
    AOS_RIGHT_READ      = (1 << 0),
    AOS_RIGHT_WRITE     = (1 << 1),
    AOS_RIGHT_EXEC      = (1 << 2),
    AOS_RIGHT_GRANT     = (1 << 3),  /* Can delegate this capability */
    AOS_RIGHT_REVOKE    = (1 << 4),  /* Can revoke derived capabilities */
    AOS_RIGHT_ALL       = 0x1F,
} aos_rights_t;

/* Agent message */
typedef struct {
    agent_id_t    sender;       /* Verified by kernel badge — unforgeable */
    uint32_t      msg_type;     /* aos_msg_type_t */
    uint32_t      schema_ver;   /* Protocol schema version */
    uint32_t      flags;        /* Message flags */
    uint32_t      payload_len;  /* Payload length in bytes */
    uint8_t       payload[];    /* Variable-length payload (flexible array) */
} aos_msg_t;

/* Channel handle */
typedef cap_t aos_channel_t;

/* Store handle */
typedef cap_t aos_store_t;

/* Tool capability */
typedef cap_t aos_tool_cap_t;

/* Model capability */
typedef cap_t aos_model_cap_t;

/* Tool definition for registration */
typedef struct {
    const char   *name;         /* Tool name (e.g. "web_search") */
    const char   *description;  /* Human/agent-readable description */
    const char   *input_schema; /* JSON Schema for input parameters */
    const char   *output_schema;/* JSON Schema for output */
    /* Tool handler function pointer */
    aos_status_t (*handler)(const uint8_t *input, size_t input_len,
                            uint8_t **output, size_t *output_len);
} aos_tool_def_t;

/* Model query parameters */
typedef struct {
    const char   *model_id;     /* Specific model to use (NULL for default) */
    float         temperature;  /* Sampling temperature */
    uint32_t      max_tokens;   /* Maximum response tokens */
    const char   *system;       /* System prompt (NULL for none) */
    uint32_t      timeout_ms;   /* Timeout in milliseconds */
} aos_model_params_t;

/* Model inference result */
typedef struct {
    aos_status_t  status;
    char         *response;     /* Generated text (caller must free) */
    uint32_t      tokens_used;
    uint32_t      tokens_prompt;
    uint64_t      latency_us;   /* Inference latency in microseconds */
} aos_inference_t;

/* Agent configuration (passed at boot) */
typedef struct {
    agent_id_t    id;
    const char   *name;         /* Agent name (for logging/debug) */
    uint32_t      trust_level;  /* 0 = untrusted, 255 = root */
    uint32_t      flags;
} aos_config_t;

/* Service interface definition (for pluggable services) */
typedef struct {
    const char   *service_id;   /* e.g. "storage.v1", "msgbus.v1" */
    const char   *interface_spec; /* CAmkES interface definition */
    uint32_t      version;
} aos_service_iface_t;

/*
 * =============================================================================
 * Agent Lifecycle
 * =============================================================================
 */

/* Initialize agent runtime. Called once at agent boot. */
aos_status_t aos_init(aos_config_t *config);

/* Graceful shutdown. Releases all capabilities, notifies system. */
aos_status_t aos_shutdown(void);

/* Get own agent ID */
agent_id_t aos_self(void);

/* Get own configuration */
const aos_config_t* aos_config(void);

/*
 * =============================================================================
 * Messaging (AgentIPC)
 * =============================================================================
 */

/* Create a named channel (requires MsgCap with CREATE right) */
aos_channel_t aos_channel_create(const char *name, uint32_t flags);

/* Open an existing channel by name */
aos_channel_t aos_channel_open(const char *name);

/* Subscribe to a channel (for async notifications) */
aos_status_t aos_channel_subscribe(aos_channel_t ch);

/* Send a message to a specific agent */
aos_status_t aos_msg_send(agent_id_t dest, aos_msg_t *msg);

/* Send a message to a channel (multicast) */
aos_status_t aos_msg_publish(aos_channel_t ch, aos_msg_t *msg);

/* Receive a message (blocks until message or timeout) */
aos_msg_t* aos_msg_recv(aos_channel_t ch, uint32_t timeout_ms);

/* Synchronous RPC call (send request, wait for reply) */
aos_msg_t* aos_msg_call(agent_id_t dest, aos_msg_t *request, uint32_t timeout_ms);

/* Free a received message */
void aos_msg_free(aos_msg_t *msg);

/* Allocate a message with payload space */
aos_msg_t* aos_msg_alloc(aos_msg_type_t type, size_t payload_len);

/*
 * =============================================================================
 * Tool Invocation
 * =============================================================================
 */

/* Register a tool (makes it available to other agents with ToolCap) */
aos_status_t aos_tool_register(aos_tool_def_t *def);

/* Unregister a tool */
aos_status_t aos_tool_unregister(const char *name);

/* Call a tool (requires ToolCap) */
aos_status_t aos_tool_call(aos_tool_cap_t cap, const char *method,
                           const uint8_t *args, size_t args_len,
                           uint8_t **result, size_t *result_len);

/* List available tools (that this agent has capabilities for) */
aos_status_t aos_tool_list(char ***names, size_t *count);

/*
 * =============================================================================
 * Model / Inference
 * =============================================================================
 */

/* Query a model (requires ModelCap) */
aos_inference_t aos_model_query(aos_model_cap_t cap, const char *prompt,
                                 aos_model_params_t *params);

/* Free inference result */
void aos_inference_free(aos_inference_t *result);

/* List available models */
aos_status_t aos_model_list(char ***model_ids, size_t *count);

/*
 * =============================================================================
 * Storage
 * =============================================================================
 */

/* Open a storage path (requires StoreCap) */
aos_store_t aos_store_open(cap_t store_cap, const char *path, uint32_t flags);

/* Read from store */
ssize_t aos_store_read(aos_store_t h, void *buf, size_t len);

/* Write to store */
ssize_t aos_store_write(aos_store_t h, const void *buf, size_t len);

/* Close store handle */
aos_status_t aos_store_close(aos_store_t h);

/* List directory / namespace */
aos_status_t aos_store_list(cap_t store_cap, const char *path,
                            char ***entries, size_t *count);

/* Delete a path */
aos_status_t aos_store_delete(cap_t store_cap, const char *path);

/* Storage open flags */
#define AOS_STORE_RDONLY    0x00
#define AOS_STORE_WRONLY    0x01
#define AOS_STORE_RDWR      0x02
#define AOS_STORE_CREATE    0x04
#define AOS_STORE_APPEND    0x08
#define AOS_STORE_TRUNC     0x10

/*
 * =============================================================================
 * Capability Management
 * =============================================================================
 */

/* Derive a new capability with reduced rights */
cap_t aos_cap_derive(cap_t parent, aos_rights_t rights);

/* Grant a capability to another agent (requires GRANT right) */
aos_status_t aos_cap_grant(agent_id_t dest, cap_t cap);

/* Revoke a derived capability (requires REVOKE right) */
aos_status_t aos_cap_revoke(cap_t cap);

/* Query own capabilities */
aos_status_t aos_cap_list(cap_t **caps, size_t *count);

/* Get capability info (type, rights, badge) */
aos_status_t aos_cap_info(cap_t cap, uint32_t *type, aos_rights_t *rights);

/*
 * =============================================================================
 * Service Hot-Swap (Vibe-Code Layer)
 * =============================================================================
 */

/* Propose a new implementation for a pluggable service */
aos_status_t aos_service_propose(const char *service_id,
                                  const void *component_image,
                                  size_t image_len,
                                  uint32_t *proposal_id);

/* Check proposal validation status */
aos_status_t aos_service_proposal_status(uint32_t proposal_id,
                                          uint32_t *validation_state);

/* Activate a validated proposal (swap the service) */
aos_status_t aos_service_swap(uint32_t proposal_id);

/* Query current service implementation info */
aos_status_t aos_service_info(const char *service_id,
                               aos_service_iface_t *info);

/* ── HTTP / Bridge communication ─────────────────────────────────────────── */

/**
 * aos_http_post — HTTP POST to a URL, using the bridge as proxy.
 * The bridge is reachable at 10.0.2.2:8790 from within the QEMU guest.
 */
aos_status_t aos_http_post(const char *url, const char *body_json,
                            char *resp_buf, size_t resp_buf_len,
                            size_t *resp_len);

/* ── JS Runtime (QuickJS) ────────────────────────────────────────────────── */

/**
 * aos_js_eval — evaluate a JavaScript string in a QuickJS context.
 *
 * @param context_id  Existing context to use; pass 0xFF to auto-create one.
 * @param script      NUL-terminated JavaScript source to evaluate.
 * @param out_buf     Caller-supplied buffer for the JSON-stringified result.
 * @param out_buf_len Size of out_buf in bytes.
 * @param out_len     Set to the number of bytes written to out_buf on success,
 *                    or to the error string length on failure.
 * @return AOS_OK on success, AOS_ERR_IO on JS error or staging not mapped,
 *         AOS_ERR_INVALID on bad arguments.
 */
aos_status_t aos_js_eval(uint32_t context_id,
                          const char *script,
                          char *out_buf, size_t out_buf_len,
                          size_t *out_len);

/**
 * aos_js_call — call a named JavaScript function in an existing context.
 *
 * @param context_id  Active JS context that contains the function.
 * @param func_name   NUL-terminated name of the global function to call.
 * @param args_json   JSON array string of arguments (NULL or "" for no args).
 * @param out_buf     Caller-supplied buffer for the JSON-encoded return value.
 * @param out_buf_len Size of out_buf in bytes.
 * @param out_len     Set to the number of bytes written on success.
 * @return AOS_OK on success, AOS_ERR_IO on JS error or staging not mapped.
 */
aos_status_t aos_js_call(uint32_t context_id,
                          const char *func_name,
                          const char *args_json,
                          char *out_buf, size_t out_buf_len,
                          size_t *out_len);

/* ── WireGuard Overlay Network ───────────────────────────────────────────── */

/**
 * aos_wg_add_peer — register a WireGuard peer with the wg_net PD.
 *
 * @param peer_id        Peer slot index (0..WG_MAX_PEERS-1).
 * @param pubkey         Curve25519 public key of the peer (32 bytes).
 * @param endpoint_ip_be IPv4 endpoint address in network byte order.
 * @param endpoint_port  UDP endpoint port in host byte order.
 * @param allowed_ip_be  Allowed-IP network address in network byte order.
 * @param allowed_mask   Subnet mask in host byte order (e.g. 0xFFFFFF00 = /24).
 * @return AOS_OK on success, AOS_ERR_IO on wg_net error or staging not mapped.
 */
aos_status_t aos_wg_add_peer(uint8_t peer_id,
                              const uint8_t pubkey[32],
                              uint32_t endpoint_ip_be,
                              uint16_t endpoint_port,
                              uint32_t allowed_ip_be,
                              uint32_t allowed_mask);

/**
 * aos_wg_send — encrypt and transmit data to a WireGuard peer.
 *
 * @param peer_id  Peer slot index identifying which session keys to use.
 * @param data     Plaintext payload to encrypt and send.
 * @param len      Length of data in bytes (max 65535).
 * @return AOS_OK on success, AOS_ERR_IO on wg_net error or staging not mapped,
 *         AOS_ERR_INVALID if len is 0 or exceeds the maximum.
 */
aos_status_t aos_wg_send(uint8_t peer_id,
                          const uint8_t *data, uint32_t len);

/**
 * aos_wg_recv — poll for a decrypted packet from a WireGuard peer.
 *
 * Non-blocking: returns AOS_OK with *recv_len == 0 if no packet is pending.
 *
 * @param peer_id   Peer to receive from; 0xFF to accept from any peer.
 * @param buf       Caller-supplied buffer for the decrypted payload.
 * @param max_len   Size of buf in bytes.
 * @param recv_len  Set to the number of bytes received (0 = no packet ready).
 * @return AOS_OK on success (even if no data), AOS_ERR_IO on wg_net error.
 */
aos_status_t aos_wg_recv(uint8_t peer_id,
                          uint8_t *buf, uint32_t max_len,
                          uint32_t *recv_len);

/* ── Vibe-coding: generation and compilation ─────────────────────────────── */

/**
 * aos_vibe_generate — ask the bridge to generate C service code via LLM.
 * On success, out_code contains null-terminated C source.
 */
aos_status_t aos_vibe_generate(const char *prompt, const char *service_id,
                                char *out_code, size_t out_code_len,
                                size_t *actual_len);

/**
 * aos_vibe_compile — ask the bridge to compile C source to WASM.
 * On success, the WASM binary is in the vibe staging region at offset 0,
 * and *wasm_size_out is set to its size in bytes.
 */
aos_status_t aos_vibe_compile(const char *source_c, const char *service_id,
                               uint32_t *wasm_size_out);

/*
 * =============================================================================
 * Utility
 * =============================================================================
 */

/* Get monotonic time in microseconds */
uint64_t aos_time_us(void);

/* Sleep for specified microseconds */
aos_status_t aos_sleep_us(uint64_t us);

/* Log a message (sent to LogSvc) */
void aos_log(uint32_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define AOS_LOG_TRACE   0
#define AOS_LOG_DEBUG   1
#define AOS_LOG_INFO    2
#define AOS_LOG_WARN    3
#define AOS_LOG_ERROR   4
#define AOS_LOG_FATAL   5

#ifdef __cplusplus
}
#endif

#endif /* AGENTOS_H */
