/*
 * agentOS ModelSvc — Model Inference Service
 *
 * Abstracts all model inference behind a capability-gated service.
 * Reference implementation proxies to HTTP APIs (OpenAI-compatible).
 * Agents with ModelCap can query any registered model endpoint.
 *
 * The design is pluggable: agents can vibe-code a replacement that
 * does local inference, quantization, model routing, ensemble voting,
 * or whatever they dream up. As long as it implements the interface.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define MODELSVC_MAX_MODELS     32
#define MODEL_ID_MAX            128
#define MODEL_ENDPOINT_MAX      512

typedef struct {
    char        model_id[MODEL_ID_MAX];
    char        endpoint_url[MODEL_ENDPOINT_MAX];
    char        api_key_env[64];     /* Environment var name for API key */
    uint32_t    context_window;
    uint32_t    max_tokens;
    float       default_temperature;
    uint32_t    flags;
    uint64_t    total_requests;
    uint64_t    total_tokens_in;
    uint64_t    total_tokens_out;
    uint64_t    total_latency_us;
} modelsvc_model_t;

/* Model registry */
static modelsvc_model_t models[MODELSVC_MAX_MODELS];
static int model_count = 0;

int modelsvc_init(void) {
    memset(models, 0, sizeof(models));
    model_count = 0;
    printf("[modelsvc] ModelSvc initialized (max %d models)\n", MODELSVC_MAX_MODELS);
    return 0;
}

/*
 * Register a model endpoint
 * 
 * models are registered by the init task from the system configuration.
 * Agents with elevated trust can also register their own model endpoints.
 */
int modelsvc_register(const char *model_id, const char *endpoint_url,
                       const char *api_key_env, uint32_t context_window,
                       uint32_t max_tokens) {
    if (model_count >= MODELSVC_MAX_MODELS) return -1;
    
    int idx = model_count++;
    strncpy(models[idx].model_id, model_id, MODEL_ID_MAX - 1);
    strncpy(models[idx].endpoint_url, endpoint_url, MODEL_ENDPOINT_MAX - 1);
    if (api_key_env) strncpy(models[idx].api_key_env, api_key_env, 63);
    models[idx].context_window = context_window;
    models[idx].max_tokens = max_tokens;
    models[idx].default_temperature = 0.7f;
    
    printf("[modelsvc] Registered model '%s' at %s\n", model_id, endpoint_url);
    return 0;
}

/*
 * Register default models from system config
 * Called by init task during boot.
 */
int modelsvc_register_defaults(void) {
    /* Default: OpenAI-compatible endpoint (configurable at boot) */
    modelsvc_register("default",
                       "https://inference-api.nvidia.com/v1/chat/completions",
                       "NVIDIA_API_KEY",
                       128000, 4096);
    
    /* Code generation model */
    modelsvc_register("code-gen",
                       "https://inference-api.nvidia.com/v1/chat/completions",
                       "NVIDIA_API_KEY",
                       128000, 32768);
    
    /* Fast/cheap model for simple tasks */
    modelsvc_register("fast",
                       "https://api.openai.com/v1/chat/completions",
                       "OPENAI_API_KEY",
                       16000, 4096);
    
    printf("[modelsvc] Default models registered\n");
    return 0;
}

/*
 * Query a model
 * 
 * In the full implementation, this:
 * 1. Validates ModelCap via CapStore
 * 2. Selects the appropriate endpoint
 * 3. Makes an HTTP request via NetStack
 * 4. Returns structured response
 *
 * TODO: Full HTTP implementation requires NetStack integration
 */
int modelsvc_query(uint8_t *requester, const char *model_id,
                    const char *system_prompt, const char *user_prompt,
                    float temperature, uint32_t max_tokens,
                    char **response, uint32_t *tokens_used) {
    /* Find model */
    int idx = -1;
    const char *target_id = model_id ? model_id : "default";
    
    for (int i = 0; i < model_count; i++) {
        if (strcmp(models[i].model_id, target_id) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        /* Fall back to default */
        idx = 0;
        if (idx >= model_count) return -1;
    }
    
    /* TODO: Validate requester has ModelCap via CapStore */
    
    /* TODO: Make HTTP request via NetStack */
    printf("[modelsvc] Query to '%s' (max_tokens=%u, temp=%.2f)\n",
           models[idx].model_id, max_tokens, temperature);
    printf("[modelsvc]   System: %.60s%s\n", 
           system_prompt ? system_prompt : "(none)",
           system_prompt && strlen(system_prompt) > 60 ? "..." : "");
    printf("[modelsvc]   Prompt: %.80s%s\n",
           user_prompt,
           strlen(user_prompt) > 80 ? "..." : "");
    
    models[idx].total_requests++;
    
    /* Placeholder response */
    static char placeholder[] = 
        "agentOS ModelSvc: HTTP inference not yet implemented. "
        "NetStack integration pending. This placeholder proves the "
        "IPC path from agent -> ModelSvc works correctly.";
    
    *response = placeholder;
    *tokens_used = 50;
    
    return 0;
}

/*
 * Get model stats (for monitoring/audit)
 */
int modelsvc_stats(const char *model_id, uint64_t *requests,
                    uint64_t *tokens_in, uint64_t *tokens_out,
                    uint64_t *avg_latency_us) {
    for (int i = 0; i < model_count; i++) {
        if (strcmp(models[i].model_id, model_id) == 0) {
            *requests = models[i].total_requests;
            *tokens_in = models[i].total_tokens_in;
            *tokens_out = models[i].total_tokens_out;
            *avg_latency_us = models[i].total_requests > 0 ?
                models[i].total_latency_us / models[i].total_requests : 0;
            return 0;
        }
    }
    return -1;
}
