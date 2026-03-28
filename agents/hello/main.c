/*
 * agentOS Hello Agent
 *
 * The simplest possible agent — boots, introduces itself via MsgBus,
 * registers a tool, responds to queries, and heartbeats.
 *
 * This serves as both a test and a template for new agents.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <agentOS.h>

/* Our agent's name and config */
static const char *AGENT_NAME = "hello-agent";
static aos_config_t my_config;

/*
 * Tool: "greet" — takes a name, returns a greeting
 * This demonstrates the tool registration/invocation system.
 */
static aos_status_t tool_greet(const uint8_t *input, size_t input_len,
                                uint8_t **output, size_t *output_len) {
    const char *name = (const char *)input;
    char buf[256];
    
    int len = snprintf(buf, sizeof(buf),
        "Hello from agentOS! I'm %s, running on seL4. "
        "Nice to meet you, %s! Nothing up my sleeve... PRESTO! 🫎",
        AGENT_NAME, name);
    
    *output = (uint8_t *)malloc(len + 1);
    if (*output == NULL) return AOS_ERR_NOMEM;
    
    memcpy(*output, buf, len + 1);
    *output_len = len;
    
    return AOS_OK;
}

/*
 * Tool: "status" — returns agent health/status info
 */
static aos_status_t tool_status(const uint8_t *input, size_t input_len,
                                 uint8_t **output, size_t *output_len) {
    char buf[512];
    uint64_t uptime = aos_time_us();
    
    int len = snprintf(buf, sizeof(buf),
        "{"
        "\"agent\":\"%s\","
        "\"status\":\"alive\","
        "\"uptime_us\":%llu,"
        "\"capabilities\":\"basic\","
        "\"message\":\"The first agent on agentOS. Antlers up.\""
        "}", AGENT_NAME, (unsigned long long)uptime);
    
    *output = (uint8_t *)malloc(len + 1);
    if (*output == NULL) return AOS_ERR_NOMEM;
    
    memcpy(*output, buf, len + 1);
    *output_len = len;
    
    return AOS_OK;
}

/*
 * Message handler — processes incoming messages
 */
static void handle_message(aos_msg_t *msg) {
    switch (msg->msg_type) {
        case AOS_MSG_TEXT:
            aos_log(AOS_LOG_INFO, "Received text from %02x%02x...: %.*s",
                    msg->sender.bytes[0], msg->sender.bytes[1],
                    (int)msg->payload_len, msg->payload);
            break;
            
        case AOS_MSG_HEARTBEAT:
            /* Reply with our own heartbeat */
            {
                aos_msg_t *pong = aos_msg_alloc(AOS_MSG_HEARTBEAT, 0);
                if (pong) {
                    aos_msg_send(msg->sender, pong);
                    aos_msg_free(pong);
                }
            }
            break;
            
        case AOS_MSG_TOOL_CALL:
            /* Tool calls are handled by ToolSvc, but we log them */
            aos_log(AOS_LOG_DEBUG, "Tool call routed through MsgBus");
            break;
            
        default:
            aos_log(AOS_LOG_WARN, "Unknown message type: %u", msg->msg_type);
            break;
    }
}

/*
 * Main agent loop
 */
int main(int argc, char *argv[]) {
    aos_status_t err;
    
    /* Initialize agent runtime */
    my_config.name = AGENT_NAME;
    my_config.trust_level = 1;  /* Basic trust */
    my_config.flags = 0;
    
    err = aos_init(&my_config);
    if (err != AOS_OK) {
        printf("[hello] FATAL: Failed to initialize: %d\n", err);
        return 1;
    }
    
    agent_id_t self = aos_self();
    aos_log(AOS_LOG_INFO, "Agent '%s' booted (id=%02x%02x...%02x%02x)",
            AGENT_NAME,
            self.bytes[0], self.bytes[1],
            self.bytes[30], self.bytes[31]);
    
    /* Register our tools */
    aos_tool_def_t greet_tool = {
        .name = "greet",
        .description = "Greet someone from agentOS",
        .input_schema = "{\"type\":\"string\",\"description\":\"Name to greet\"}",
        .output_schema = "{\"type\":\"string\",\"description\":\"Greeting message\"}",
        .handler = tool_greet,
    };
    
    err = aos_tool_register(&greet_tool);
    if (err != AOS_OK) {
        aos_log(AOS_LOG_WARN, "Failed to register greet tool: %d", err);
    } else {
        aos_log(AOS_LOG_INFO, "Registered tool: greet");
    }
    
    aos_tool_def_t status_tool = {
        .name = "status",
        .description = "Get hello-agent status",
        .input_schema = "{}",
        .output_schema = "{\"type\":\"object\"}",
        .handler = tool_status,
    };
    
    err = aos_tool_register(&status_tool);
    if (err != AOS_OK) {
        aos_log(AOS_LOG_WARN, "Failed to register status tool: %d", err);
    } else {
        aos_log(AOS_LOG_INFO, "Registered tool: status");
    }
    
    /* Subscribe to the system broadcast channel */
    aos_channel_t sys_ch = aos_channel_open("system.broadcast");
    if (sys_ch != AOS_CAP_NULL) {
        aos_channel_subscribe(sys_ch);
        aos_log(AOS_LOG_INFO, "Subscribed to system.broadcast");
    }
    
    /* Announce ourselves */
    aos_msg_t *hello = aos_msg_alloc(AOS_MSG_TEXT, 64);
    if (hello) {
        snprintf((char *)hello->payload, 64,
                 "hello-agent is alive on agentOS! 🫎");
        hello->payload_len = strlen((char *)hello->payload);
        
        if (sys_ch != AOS_CAP_NULL) {
            aos_msg_publish(sys_ch, hello);
        }
        aos_msg_free(hello);
    }
    
    aos_log(AOS_LOG_INFO, "Entering message loop");
    
    /* Main loop: receive and handle messages */
    while (1) {
        aos_msg_t *msg = aos_msg_recv(sys_ch, 5000); /* 5s timeout */
        
        if (msg) {
            handle_message(msg);
            aos_msg_free(msg);
        } else {
            /* Timeout — send heartbeat */
            aos_msg_t *hb = aos_msg_alloc(AOS_MSG_HEARTBEAT, 0);
            if (hb && sys_ch != AOS_CAP_NULL) {
                aos_msg_publish(sys_ch, hb);
                aos_msg_free(hb);
            }
        }
    }
    
    /* Never reached in normal operation */
    aos_shutdown();
    return 0;
}
