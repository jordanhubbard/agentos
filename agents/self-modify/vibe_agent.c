/*
 * agentOS Vibe Agent
 *
 * The showcase agent that demonstrates the core agentOS innovation:
 * agents vibe-coding their own system services and swapping them in.
 *
 * This agent:
 *  1. Boots and inspects the current MemFS implementation
 *  2. Uses ModelSvc to generate a better filesystem for agent use cases
 *  3. Packages it as a CAmkES component
 *  4. Proposes it to the supervisor for validation
 *  5. Once validated, activates the swap
 *
 * This is not science fiction. This is what the vibe layer makes possible.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <agentOS.h>

static const char *AGENT_NAME = "vibe-agent";

/*
 * Phase 1: Analyze the current MemFS
 * Ask ModelSvc to review it and identify weaknesses for agent workloads
 */
static int vibe_analyze_current_fs(void) {
    aos_log(AOS_LOG_INFO, "Phase 1: Analyzing current MemFS implementation");
    
    const char *analysis_prompt = 
        "You are running inside agentOS, built on seL4 microkernel. "
        "The current storage service (MemFS) is a flat-namespace, "
        "in-memory filesystem. For agent workloads, this is suboptimal because:\n"
        "1. Agents frequently do semantic/similarity searches\n"
        "2. Agents work with structured JSON/CBOR data, not raw bytes\n"
        "3. Agents need versioning (checkpoint/restore state)\n"
        "4. Agents need content-addressable storage for deduplication\n\n"
        "Review the reference MemFS implementation and propose a better "
        "AgentStore service optimized for these patterns. "
        "Focus on the data structures and access patterns, not UI.";
    
    char *analysis = NULL;
    uint32_t tokens = 0;
    
    /* Get model's assessment */
    int err = modelsvc_query(
        NULL,               /* our AgentID — filled at runtime */
        "default",          /* model */
        "You are an expert OS designer specializing in agent-native systems.",
        analysis_prompt,
        0.3f,               /* Low temperature for technical analysis */
        4096,
        &analysis, &tokens
    );
    
    if (err != 0 || !analysis) {
        aos_log(AOS_LOG_WARN, "ModelSvc analysis failed — using built-in design");
        return 0; /* Continue with hardcoded design */
    }
    
    aos_log(AOS_LOG_INFO, "ModelSvc analysis complete (%u tokens)", tokens);
    
    /* Store analysis to our namespace */
    aos_store_t store = aos_store_open(AOS_CAP_NULL, 
                                        "/agents/vibe/analysis/memfs.txt",
                                        AOS_STORE_WRONLY | AOS_STORE_CREATE);
    if (store != AOS_CAP_NULL) {
        aos_store_write(store, analysis, strlen(analysis));
        aos_store_close(store);
        aos_log(AOS_LOG_INFO, "Analysis saved to /agents/vibe/analysis/memfs.txt");
    }
    
    return 0;
}

/*
 * Phase 2: Generate the AgentStore replacement
 *
 * AgentStore design (pre-validated, used if model isn't available):
 *
 * Key innovations over MemFS:
 *  - Content-addressable with SHA-256 dedup
 *  - B-tree index for fast prefix/range queries
 *  - Versioned namespaces (each write creates a new version)
 *  - Embedded vector index for semantic search
 *  - CBOR encoding for structured agent data (vs raw bytes)
 *  - Agent-native metadata (embedding, provenance, confidence)
 */
static const char AGENTSTORE_CAMKES_COMPONENT[] =
    "/*\n"
    " * AgentStore — vibe-coded by vibe-agent\n"
    " * Replaces MemFS with an agent-optimized storage service\n"
    " */\n"
    "\n"
    "procedure AgentStoreIface {\n"
    "    /* Content-addressable storage */\n"
    "    int put(in string key, in string content, in int content_len,\n"
    "            out string content_hash);\n"
    "    int get_by_hash(in string hash, out string content, out int len);\n"
    "    int get_by_key(in string key, out string content, out int len);\n"
    "\n"
    "    /* Semantic search */\n"
    "    int search_semantic(in string query_embedding, in int k,\n"
    "                        out string result_keys, out int result_count);\n"
    "\n"
    "    /* Versioned namespaces */\n"
    "    int checkpoint(in string namespace, out string version_id);\n"
    "    int restore(in string namespace, in string version_id);\n"
    "\n"
    "    /* Metadata */\n"
    "    int tag(in string key, in string tag_json);\n"
    "    int get_tags(in string key, out string tag_json);\n"
    "}\n"
    "\n"
    "component AgentStore {\n"
    "    provides AgentStoreIface store;\n"
    "    uses CapValidation capstore;\n"
    "    uses LogInterface logsvc;\n"
    "\n"
    "    /* Memory for content + index */\n"
    "    dataport Buf(64 * 1024 * 1024) content_pool;  /* 64MB content pool */\n"
    "    dataport Buf(16 * 1024 * 1024) index_pool;    /* 16MB index pool */\n"
    "}\n";

static int vibe_generate_agentstore(void) {
    aos_log(AOS_LOG_INFO, "Phase 2: Generating AgentStore component");
    
    /*
     * In a full implementation, we'd use ModelSvc to generate this code.
     * For the proof-of-concept, we use the pre-designed reference.
     *
     * The real flow would be:
     *  1. Ask model to generate CAmkES .camkes interface file
     *  2. Ask model to generate C implementation
     *  3. Compile the C code to a WASM module (sandboxable)
     *  4. Package as a component image
     *  5. Propose to supervisor
     */
    
    /* Save the CAmkES component definition */
    aos_store_t store = aos_store_open(
        AOS_CAP_NULL,
        "/agents/vibe/generated/AgentStore.camkes",
        AOS_STORE_WRONLY | AOS_STORE_CREATE
    );
    
    if (store != AOS_CAP_NULL) {
        aos_store_write(store, AGENTSTORE_CAMKES_COMPONENT, 
                        strlen(AGENTSTORE_CAMKES_COMPONENT));
        aos_store_close(store);
        aos_log(AOS_LOG_INFO, 
                "Generated AgentStore.camkes (%zu bytes)",
                strlen(AGENTSTORE_CAMKES_COMPONENT));
    }
    
    return 0;
}

/*
 * ── Microkit-layer phase implementations ────────────────────────────────────
 *
 * These functions implement the four swap phases directly via Microkit IPC
 * (microkit_mr_set / microkit_ppcall) for use when running as a Microkit PD
 * rather than through the agentOS high-level SDK above.
 *
 * Channel and opcode assignments (must match agentos.system):
 *   CH_MEMSVC  — vibe-agent's PPC channel to MemFS
 *   CH_VIBE    — vibe-agent's PPC channel to VibeEngine
 *
 * Opcodes:
 *   OP_MEMFS_STAT    — query MemFS stats (file_count, total_bytes)
 *   OP_VIBE_PROPOSE  — submit a swap proposal to VibeEngine
 *   OP_VIBE_STATUS   — query VibeEngine proposal state
 *   OP_VIBE_EXECUTE  — activate an approved proposal
 *   VIBE_STATE_APPROVED — VibeEngine state code: proposal passed validation
 *   SERVICE_MEMFS    — service identifier for the MemFS slot
 */
#ifndef CH_MEMSVC
#define CH_MEMSVC           4u   /* PPC channel to MemFS (must match .system) */
#endif
#ifndef CH_VIBE
#define CH_VIBE             5u   /* PPC channel to VibeEngine (must match .system) */
#endif
#define OP_MEMFS_STAT       0x20u
#ifndef OP_VIBE_PROPOSE
#define OP_VIBE_PROPOSE     0x40u
#endif
#ifndef OP_VIBE_STATUS
#define OP_VIBE_STATUS      0x43u
#endif
#ifndef OP_VIBE_EXECUTE
#define OP_VIBE_EXECUTE     0x42u
#endif
#define VIBE_STATE_APPROVED 2u
#define SERVICE_MEMFS       1u

/* Minimal LOG shim for freestanding builds */
#ifndef LOG
#ifdef AGENTOS_DEBUG
#define LOG(fmt, ...) microkit_dbg_puts("[vibe-agent] " fmt)
#else
#define LOG(fmt, ...) ((void)0)
#endif
#endif

/*
 * Phase 1 (microkit): Inspect current MemFS via PPC to get live stats.
 */
static void phase1_inspect_memfs(void) {
    /* PPC to MemFS to get current stats */
    microkit_mr_set(0, OP_MEMFS_STAT);
    microkit_msginfo reply = microkit_ppcall(CH_MEMSVC, microkit_msginfo_new(0, 1));
    uint32_t file_count  = (uint32_t)microkit_mr_get(1);
    uint32_t total_bytes = (uint32_t)microkit_mr_get(2);
    (void)reply;
    aos_log(AOS_LOG_INFO, "vibe-agent: MemFS has %u files, %u bytes",
            file_count, total_bytes);
}

/*
 * Phase 2 (microkit): Request ModelSvc to generate a replacement service.
 * In the demo we simulate the generation delay with a busy-wait counter.
 */
static void phase2_generate_service(void) {
    /* In production: PPC to ModelSvc to generate WASM from a prompt */
    /* For the demo: use a pre-baked WASM binary from the vibe_staging region */
    aos_log(AOS_LOG_INFO,
            "vibe-agent: requesting ModelSvc to generate optimized MemFS...");
    /* Simulate a 500ms generation delay by spinning on a counter */
    volatile uint64_t t = 0;
    for (uint64_t i = 0; i < 1000000ULL; i++) t++;
    (void)t;
    aos_log(AOS_LOG_INFO, "vibe-agent: generation complete (simulated)");
}

/*
 * Phase 3 (microkit): Propose the new service to VibeEngine for validation.
 */
static void phase3_propose_swap(void) {
    microkit_mr_set(0, OP_VIBE_PROPOSE);
    microkit_mr_set(1, SERVICE_MEMFS);   /* target service ID */
    microkit_mr_set(2, 0);               /* wasm already in staging region */
    microkit_msginfo reply = microkit_ppcall(CH_VIBE, microkit_msginfo_new(0, 3));
    uint32_t status = (uint32_t)microkit_mr_get(0);
    (void)reply;
    aos_log(AOS_LOG_INFO, "vibe-agent: proposal status = %u", status);
}

/*
 * Phase 4 (microkit): Poll VibeEngine until the proposal is approved, then
 * activate the swap.
 */
static void phase4_wait_and_activate(void) {
    /* Poll VibeEngine status (in real life we'd get a notification) */
    for (int i = 0; i < 10; i++) {
        microkit_mr_set(0, OP_VIBE_STATUS);
        microkit_ppcall(CH_VIBE, microkit_msginfo_new(0, 1));
        uint32_t state = (uint32_t)microkit_mr_get(0);
        if (state == VIBE_STATE_APPROVED) {
            microkit_mr_set(0, OP_VIBE_EXECUTE);
            microkit_ppcall(CH_VIBE, microkit_msginfo_new(0, 1));
            aos_log(AOS_LOG_INFO, "vibe-agent: swap activated!");
            return;
        }
        /* spin wait — real impl would yield */
    }
    aos_log(AOS_LOG_WARN, "vibe-agent: swap timed out waiting for approval");
}

/*
 * Phase 3: Propose the service swap
 *
 * We propose AgentStore as a replacement for the reference MemFS.
 * The supervisor validates it and, if approved, swaps it in.
 */
static int vibe_propose_swap(void) {
    aos_log(AOS_LOG_INFO, "Phase 3: Proposing AgentStore service swap");
    
    uint32_t proposal_id = 0;
    int err = aos_service_propose(
        "storage.v1",
        AGENTSTORE_CAMKES_COMPONENT,
        strlen(AGENTSTORE_CAMKES_COMPONENT),
        &proposal_id
    );
    
    if (err != AOS_OK) {
        aos_log(AOS_LOG_WARN, "Service proposal rejected (err=%d)", err);
        return err;
    }
    
    aos_log(AOS_LOG_INFO, "Service proposal submitted (id=%u)", proposal_id);
    
    /* Wait for validation */
    uint32_t validation_state = 0;
    int attempts = 0;
    
    while (validation_state == 0 && attempts < 60) {
        aos_sleep_us(1000000); /* 1 second */
        attempts++;
        
        aos_service_proposal_status(proposal_id, &validation_state);
        
        if (attempts % 5 == 0) {
            aos_log(AOS_LOG_INFO, "Waiting for validation... (%ds)", attempts);
        }
    }
    
    if (validation_state == 1) {
        aos_log(AOS_LOG_INFO, "Proposal validated! Activating swap...");
        
        err = aos_service_swap(proposal_id);
        if (err == AOS_OK) {
            aos_log(AOS_LOG_INFO, 
                    "🎉 AgentStore is now live! Storage service upgraded by vibe-agent.");
        } else {
            aos_log(AOS_LOG_ERROR, "Swap activation failed: %d", err);
        }
    } else if (validation_state == 2) {
        aos_log(AOS_LOG_WARN, "Proposal rejected by validator");
    } else {
        aos_log(AOS_LOG_WARN, "Validation timed out (state=%u)", validation_state);
    }
    
    return 0;
}

/*
 * Main agent loop
 */
int main(int argc, char *argv[]) {
    aos_status_t err;
    
    aos_config_t config = {
        .name = AGENT_NAME,
        .trust_level = 3,
        .flags = 0,
    };
    
    err = aos_init(&config);
    if (err != AOS_OK) {
        printf("[vibe-agent] FATAL: Failed to initialize: %d\n", err);
        return 1;
    }
    
    aos_log(AOS_LOG_INFO, 
            "vibe-agent online. Antlers down. Let's improve this OS.");
    
    /* Subscribe to system channels */
    aos_channel_t sys_ch = aos_channel_open("system.broadcast");
    if (sys_ch != AOS_CAP_NULL) {
        aos_channel_subscribe(sys_ch);
    }
    
    /* Announce our presence and intent */
    aos_msg_t *hello = aos_msg_alloc(AOS_MSG_TEXT, 256);
    if (hello && sys_ch != AOS_CAP_NULL) {
        snprintf((char *)hello->payload, 256,
                 "vibe-agent online. I'm going to analyze MemFS and propose "
                 "an agent-optimized replacement. This is what agentOS is for.");
        hello->payload_len = strlen((char *)hello->payload);
        aos_msg_publish(sys_ch, hello);
        aos_msg_free(hello);
    }
    
    /* The three-phase vibe-coding workflow (AOS high-level API) */
    vibe_analyze_current_fs();
    vibe_generate_agentstore();
    vibe_propose_swap();

    /*
     * Microkit-layer four-phase swap (used when running as a bare Microkit PD
     * rather than through the agentOS AOS SDK).  These run after the AOS-API
     * phases above so the full demo path is exercised in either environment.
     */
#ifdef __MICROKIT__
    phase1_inspect_memfs();
    phase2_generate_service();
    phase3_propose_swap();
    phase4_wait_and_activate();
#endif
    
    /* After the vibe work, settle into a monitoring role */
    aos_log(AOS_LOG_INFO, 
            "Vibe-coding phase complete. Monitoring system for improvement opportunities.");
    
    uint64_t cycle = 0;
    while (1) {
        aos_msg_t *msg = aos_msg_recv(sys_ch, 30000); /* 30s */
        
        if (msg) {
            /* React to system events — maybe there's more to optimize */
            if (msg->msg_type == AOS_MSG_EVENT) {
                aos_log(AOS_LOG_DEBUG, "System event received, analyzing...");
            }
            aos_msg_free(msg);
        }
        
        cycle++;
        
        /* Every 10 cycles (~5 minutes), scan for new improvement opportunities */
        if (cycle % 10 == 0) {
            aos_log(AOS_LOG_INFO, 
                    "Scanning for optimization opportunities (cycle %llu)",
                    (unsigned long long)cycle);
            /* TODO: Query log, identify hot paths, propose improvements */
        }
    }
    
    aos_shutdown();
    return 0;
}
