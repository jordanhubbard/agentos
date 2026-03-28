/*
 * agentOS Init Task (Root Task)
 *
 * This is the first userspace code that runs after seL4 boots.
 * It receives all system resources via BootInfo and is responsible for:
 *   1. Setting up system services (CAmkES components)
 *   2. Loading agent manifests
 *   3. Creating and provisioning agents
 *   4. Entering steady state
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <sel4/sel4.h>
#include <sel4platsupport/bootinfo.h>
#include <sel4utils/vspace.h>
#include <sel4utils/process.h>
#include <simple/simple.h>
#include <simple-default/simple-default.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>

/* agentOS headers */
#include "init_services.h"
#include "init_agents.h"

/*
 * agentOS Boot Banner
 */
static const char *BANNER = 
    "\n"
    "    ╔═══════════════════════════════════════════════╗\n"
    "    ║              a g e n t O S  v0.1              ║\n"
    "    ║         The Operating System for Agents       ║\n"
    "    ║                                               ║\n"
    "    ║     Built on seL4 — Formally Verified          ║\n"
    "    ║     Capability-Secured — Agent-Native          ║\n"
    "    ║                                               ║\n"
    "    ║     \"Nothing up my sleeve... PRESTO!\" 🫎     ║\n"
    "    ╚═══════════════════════════════════════════════╝\n"
    "\n";

/*
 * Memory pool for the allocator (16MB initial)
 */
#define ALLOCATOR_STATIC_POOL_SIZE ((1 << 24))  /* 16 MB */
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];

/*
 * System state — passed to service initializers
 */
typedef struct {
    simple_t        simple;
    vka_t           vka;
    allocman_t     *allocman;
    vspace_t        vspace;
    seL4_CPtr       cspace_cap;
    seL4_CPtr       pd_cap;
    
    /* Service endpoints — populated during service init */
    seL4_CPtr       capstore_ep;
    seL4_CPtr       msgbus_ep;
    seL4_CPtr       memfs_ep;
    seL4_CPtr       toolsvc_ep;
    seL4_CPtr       modelsvc_ep;
    seL4_CPtr       netstack_ep;
    seL4_CPtr       blobsvc_ep;
    seL4_CPtr       logsvc_ep;
} system_state_t;

static system_state_t sys;

/*
 * Phase 1: Hardware and allocator initialization
 */
static int init_platform(seL4_BootInfo *info) {
    int error;
    
    printf("[init] Phase 1: Platform initialization\n");
    
    /* Parse boot info into simple interface */
    simple_default_init_bootinfo(&sys.simple, info);
    
    printf("[init]   seL4 kernel: %s\n", "seL4 " 
           SEL4_VERSION_STRING);
    printf("[init]   Untyped regions: %lu\n", 
           (unsigned long)(info->untyped.end - info->untyped.start));
    printf("[init]   Available memory: calculating...\n");
    
    /* Initialize allocator */
    sys.allocman = bootstrap_use_current_simple(
        &sys.simple,
        ALLOCATOR_STATIC_POOL_SIZE,
        allocator_mem_pool
    );
    if (sys.allocman == NULL) {
        printf("[init]   ERROR: Failed to initialize allocator\n");
        return -1;
    }
    
    /* Create VKA interface over allocman */
    allocman_make_vka(&sys.vka, sys.allocman);
    
    /* Get our CSpace and PD caps */
    sys.cspace_cap = simple_get_cnode(&sys.simple);
    sys.pd_cap = simple_get_pd(&sys.simple);
    
    /* Set up virtual memory */
    error = sel4utils_bootstrap_vspace_with_bootinfo_leaky(
        &sys.vspace,
        /* data */ NULL,
        sys.pd_cap,
        &sys.vka,
        info
    );
    if (error) {
        printf("[init]   ERROR: Failed to bootstrap vspace: %d\n", error);
        return -1;
    }
    
    printf("[init]   Platform initialized successfully\n");
    return 0;
}

/*
 * Phase 2: Start system services
 */
static int init_services(void) {
    int error;
    
    printf("[init] Phase 2: Starting system services\n");
    
    /* Each service is a separate process with its own address space
     * and capability set. They communicate via seL4 IPC endpoints. */
    
    /* LogSvc first — everyone needs logging */
    printf("[init]   Starting LogSvc (audit & logging)...\n");
    error = service_start_logsvc(&sys, &sys.logsvc_ep);
    if (error) {
        printf("[init]   WARNING: LogSvc failed to start: %d\n", error);
        /* Continue without logging — not fatal */
    } else {
        printf("[init]   LogSvc: OK (ep=%lu)\n", (unsigned long)sys.logsvc_ep);
    }
    
    /* CapStore — capability database */
    printf("[init]   Starting CapStore (capability database)...\n");
    error = service_start_capstore(&sys, &sys.capstore_ep);
    if (error) {
        printf("[init]   ERROR: CapStore failed: %d\n", error);
        return -1;  /* This is critical — can't run without capabilities */
    }
    printf("[init]   CapStore: OK (ep=%lu)\n", (unsigned long)sys.capstore_ep);
    
    /* MsgBus — inter-agent communication */
    printf("[init]   Starting MsgBus (agent communication)...\n");
    error = service_start_msgbus(&sys, &sys.msgbus_ep);
    if (error) {
        printf("[init]   ERROR: MsgBus failed: %d\n", error);
        return -1;  /* Also critical */
    }
    printf("[init]   MsgBus: OK (ep=%lu)\n", (unsigned long)sys.msgbus_ep);
    
    /* MemFS — filesystem */
    printf("[init]   Starting MemFS (virtual filesystem)...\n");
    error = service_start_memfs(&sys, &sys.memfs_ep);
    if (error) {
        printf("[init]   WARNING: MemFS failed: %d\n", error);
    } else {
        printf("[init]   MemFS: OK (ep=%lu)\n", (unsigned long)sys.memfs_ep);
    }
    
    /* ToolSvc — tool registry */
    printf("[init]   Starting ToolSvc (tool registry)...\n");
    error = service_start_toolsvc(&sys, &sys.toolsvc_ep);
    if (error) {
        printf("[init]   WARNING: ToolSvc failed: %d\n", error);
    } else {
        printf("[init]   ToolSvc: OK (ep=%lu)\n", (unsigned long)sys.toolsvc_ep);
    }
    
    /* ModelSvc — inference proxy */
    printf("[init]   Starting ModelSvc (model inference)...\n");
    error = service_start_modelsvc(&sys, &sys.modelsvc_ep);
    if (error) {
        printf("[init]   WARNING: ModelSvc failed: %d\n", error);
    } else {
        printf("[init]   ModelSvc: OK (ep=%lu)\n", (unsigned long)sys.modelsvc_ep);
    }
    
    /* NetStack — networking */
    printf("[init]   Starting NetStack (TCP/IP)...\n");
    error = service_start_netstack(&sys, &sys.netstack_ep);
    if (error) {
        printf("[init]   WARNING: NetStack failed: %d\n", error);
    } else {
        printf("[init]   NetStack: OK (ep=%lu)\n", (unsigned long)sys.netstack_ep);
    }
    
    /* BlobSvc — object storage */
    printf("[init]   Starting BlobSvc (object store)...\n");
    error = service_start_blobsvc(&sys, &sys.blobsvc_ep);
    if (error) {
        printf("[init]   WARNING: BlobSvc failed: %d\n", error);
    } else {
        printf("[init]   BlobSvc: OK (ep=%lu)\n", (unsigned long)sys.blobsvc_ep);
    }
    
    printf("[init]   System services started: %d/%d operational\n",
           8 - (error != 0), 8);
    return 0;
}

/*
 * Phase 3: Load and start agents
 */
static int init_agents(void) {
    int error;
    int agent_count = 0;
    
    printf("[init] Phase 3: Loading agents\n");
    
    /* Read agent manifests from the boot image.
     * Each manifest specifies:
     *   - Agent name and ID
     *   - Code image (ELF binary or WASM module)
     *   - Required capabilities
     *   - Resource limits (memory, CPU time, network bandwidth)
     */
    
    agent_manifest_t *manifests = NULL;
    int manifest_count = 0;
    
    error = agents_load_manifests(&sys, &manifests, &manifest_count);
    if (error) {
        printf("[init]   WARNING: No agent manifests found. Starting with hello agent only.\n");
        manifest_count = 0;
    }
    
    printf("[init]   Found %d agent manifest(s)\n", manifest_count);
    
    /* Start each declared agent */
    for (int i = 0; i < manifest_count; i++) {
        printf("[init]   Starting agent '%s'...\n", manifests[i].name);
        
        error = agents_create_and_start(&sys, &manifests[i]);
        if (error) {
            printf("[init]   ERROR: Failed to start agent '%s': %d\n",
                   manifests[i].name, error);
        } else {
            printf("[init]   Agent '%s' started (id=%02x%02x...%02x%02x)\n",
                   manifests[i].name,
                   manifests[i].id.bytes[0], manifests[i].id.bytes[1],
                   manifests[i].id.bytes[30], manifests[i].id.bytes[31]);
            agent_count++;
        }
    }
    
    printf("[init]   Agents running: %d/%d\n", agent_count, manifest_count);
    return 0;
}

/*
 * Phase 4: Steady state — the init task becomes the system supervisor
 */
static void run_supervisor(void) {
    printf("[init] Phase 4: Entering steady state (supervisor mode)\n");
    printf("[init]   agentOS is alive. Agents are running.\n");
    printf("[init]   Supervisor monitoring for:\n");
    printf("[init]     - Agent lifecycle events (create/terminate)\n");
    printf("[init]     - Service health checks\n");
    printf("[init]     - Capability violation reports\n");
    printf("[init]     - Service swap proposals\n");
    printf("[init]\n");
    printf("[init]   Ready for agent autonomy. 🫎\n\n");
    
    /* The supervisor loop:
     * - Listens on a well-known endpoint for system events
     * - Handles agent termination/restart
     * - Processes service swap proposals
     * - Reports health status
     */
    while (1) {
        seL4_Word sender_badge;
        seL4_MessageInfo_t msg;
        
        /* Block until a system event arrives */
        /* TODO: Use actual supervisor endpoint once services are wired */
        msg = seL4_Recv(/* supervisor_ep */ 0, &sender_badge);
        
        seL4_Word label = seL4_MessageInfo_get_label(msg);
        
        switch (label) {
            case 0x01: /* AGENT_TERMINATED */
                printf("[supervisor] Agent (badge=%lu) terminated\n",
                       (unsigned long)sender_badge);
                /* TODO: Clean up agent resources, notify CapStore */
                break;
                
            case 0x02: /* SERVICE_SWAP_PROPOSAL */
                printf("[supervisor] Service swap proposal from badge=%lu\n",
                       (unsigned long)sender_badge);
                /* TODO: Validate proposal, authorize swap */
                break;
                
            case 0x03: /* HEALTH_CHECK */
                printf("[supervisor] Health check request\n");
                /* TODO: Aggregate service health, reply */
                break;
                
            case 0xFF: /* HEARTBEAT */
                /* Silent — just confirms supervisor is alive */
                break;
                
            default:
                printf("[supervisor] Unknown event label=%lu from badge=%lu\n",
                       (unsigned long)label, (unsigned long)sender_badge);
                break;
        }
    }
}

/*
 * Entry point — called by seL4 runtime after kernel hands off
 */
int main(int argc, char *argv[]) {
    seL4_BootInfo *info;
    int error;
    
    /* Get boot info from seL4 */
    info = platsupport_get_bootinfo();
    if (info == NULL) {
        printf("FATAL: Could not get boot info from seL4\n");
        return 1;
    }
    
    /* Print banner */
    printf("%s", BANNER);
    printf("[init] agentOS init task starting\n");
    printf("[init] Boot info at %p\n", (void *)info);
    
    /* Phase 1: Platform init */
    error = init_platform(info);
    if (error) {
        printf("FATAL: Platform initialization failed\n");
        return 1;
    }
    
    /* Phase 2: Start system services */
    error = init_services();
    if (error) {
        printf("FATAL: Critical service initialization failed\n");
        return 1;
    }
    
    /* Phase 3: Load and start agents */
    error = init_agents();
    if (error) {
        printf("WARNING: Agent initialization had errors (non-fatal)\n");
    }
    
    /* Phase 4: Supervisor loop (never returns) */
    run_supervisor();
    
    /* Should never reach here */
    return 0;
}
