/*
 * agentOS ToolSvc — Tool Registry & Dispatch Service
 *
 * Manages the registry of callable tools across the system.
 * Agents register tools, other agents invoke them via capabilities.
 * MCP-compatible interface for model-tool interaction.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define TOOLSVC_MAX_TOOLS 512
#define TOOL_NAME_MAX     128
#define TOOL_DESC_MAX     512
#define TOOL_SCHEMA_MAX   2048

typedef struct {
    char        name[TOOL_NAME_MAX];
    char        description[TOOL_DESC_MAX];
    char        input_schema[TOOL_SCHEMA_MAX];
    char        output_schema[TOOL_SCHEMA_MAX];
    uint8_t     provider[32];        /* AgentID of the tool provider */
    uint64_t    provider_badge;      /* seL4 badge for routing */
    uint32_t    flags;
    uint64_t    call_count;          /* Usage statistics */
    uint64_t    total_latency_us;
    uint64_t    registered_at;
} toolsvc_entry_t;

static toolsvc_entry_t tools[TOOLSVC_MAX_TOOLS];
static int tool_count = 0;

int toolsvc_init(void) {
    memset(tools, 0, sizeof(tools));
    tool_count = 0;
    printf("[toolsvc] ToolSvc initialized (max %d tools)\n", TOOLSVC_MAX_TOOLS);
    return 0;
}

int toolsvc_register(uint8_t *provider, const char *name, const char *desc,
                      const char *input_schema, const char *output_schema,
                      uint64_t badge) {
    if (tool_count >= TOOLSVC_MAX_TOOLS) return -1;
    
    /* Check for duplicate */
    for (int i = 0; i < tool_count; i++) {
        if (strcmp(tools[i].name, name) == 0 &&
            memcmp(tools[i].provider, provider, 32) == 0) {
            return -2; /* Already registered by same provider */
        }
    }
    
    int idx = tool_count++;
    strncpy(tools[idx].name, name, TOOL_NAME_MAX - 1);
    if (desc) strncpy(tools[idx].description, desc, TOOL_DESC_MAX - 1);
    if (input_schema) strncpy(tools[idx].input_schema, input_schema, TOOL_SCHEMA_MAX - 1);
    if (output_schema) strncpy(tools[idx].output_schema, output_schema, TOOL_SCHEMA_MAX - 1);
    memcpy(tools[idx].provider, provider, 32);
    tools[idx].provider_badge = badge;
    tools[idx].flags = 0;
    tools[idx].call_count = 0;
    tools[idx].total_latency_us = 0;
    
    printf("[toolsvc] Registered tool '%s' (provider badge=%llu)\n",
           name, (unsigned long long)badge);
    return 0;
}

int toolsvc_unregister(uint8_t *provider, const char *name) {
    for (int i = 0; i < tool_count; i++) {
        if (strcmp(tools[i].name, name) == 0 &&
            memcmp(tools[i].provider, provider, 32) == 0) {
            if (i < tool_count - 1) {
                memcpy(&tools[i], &tools[tool_count - 1], sizeof(toolsvc_entry_t));
            }
            tool_count--;
            printf("[toolsvc] Unregistered tool '%s'\n", name);
            return 0;
        }
    }
    return -1;
}

int toolsvc_dispatch(uint8_t *caller, const char *tool_name,
                      const uint8_t *input, uint32_t input_len,
                      uint8_t **output, uint32_t *output_len) {
    /* Find the tool */
    int idx = -1;
    for (int i = 0; i < tool_count; i++) {
        if (strcmp(tools[i].name, tool_name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1; /* NOT FOUND */
    
    /* TODO: Validate caller has ToolCap for this tool (via CapStore) */
    
    /* Route the call to the provider agent via IPC */
    /* The provider agent's badge tells us which endpoint to use */
    printf("[toolsvc] Dispatching '%s' call (input=%u bytes) to provider\n",
           tool_name, input_len);
    
    /* TODO: Actual IPC to provider, wait for response */
    tools[idx].call_count++;
    
    return 0;
}

/* List tools (MCP-compatible format) */
int toolsvc_list_json(uint8_t *requester, char *buf, uint32_t buf_size) {
    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, "{\"tools\":[");
    
    for (int i = 0; i < tool_count; i++) {
        /* TODO: Filter by requester's capabilities */
        if (i > 0) pos += snprintf(buf + pos, buf_size - pos, ",");
        pos += snprintf(buf + pos, buf_size - pos,
            "{\"name\":\"%s\",\"description\":\"%s\","
            "\"inputSchema\":%s,\"calls\":%llu}",
            tools[i].name, tools[i].description,
            tools[i].input_schema[0] ? tools[i].input_schema : "{}",
            (unsigned long long)tools[i].call_count);
    }
    
    pos += snprintf(buf + pos, buf_size - pos, "]}");
    return pos;
}
