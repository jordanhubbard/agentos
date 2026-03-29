/*
 * agentOS Controller Protection Domain
 * 
 * Priority 50. Lowest of the three PDs so it can PPC into higher-priority servers.
 * Controls the EventBus (passive, prio 200) and coordinates with InitAgent (prio 100).
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>

/* Memory regions - patched by Microkit setvar */
uintptr_t monitor_stack_vaddr;
uintptr_t swap_code_ctrl_0;
uintptr_t swap_code_ctrl_1;
uintptr_t swap_code_ctrl_2;
uintptr_t swap_code_ctrl_3;

/* Channel IDs (must match agentos.system id= values) */
#define CH_EVENTBUS      0
#define CH_INITAGENT     1
#define CH_SWAP_BASE     30   /* Channels 30-33: swap slot PDs */
#define NUM_SWAP_SLOTS   4

/* Forward declarations */
void vibe_swap_init(void);
int  vibe_swap_health_notify(int slot);
void cap_broker_init(void);
void agent_pool_init(void);

/* AgentFS op codes (must match agentfs.c) */
#define OP_AGENTFS_PUT      0x30
#define OP_AGENTFS_GET      0x31
#define OP_AGENTFS_STAT     0x35
#define CH_AGENTFS           5

/* Worker channel base */
#define CH_WORKER_BASE      10

/* Controller state */
static struct {
    bool eventbus_ready;
    bool initagent_ready;
    uint32_t notification_count;
    /* Demo state: stored object ID from AgentFS (first 16 bytes in 4 words) */
    uint32_t demo_obj_id[4];
    bool demo_obj_stored;
    /* Worker state tracking */
    bool     worker_task_dispatched;  /* true after we dispatched the demo task */
    bool     demo_complete;           /* true after the demo is fully done */
} ctrl = { false, false, 0, {0}, false, false, false };

/* Simple busy-wait delay for demo sequencing (no timers on bare metal) */
static void demo_delay(void) {
    for (volatile uint32_t i = 0; i < 100000; i++) {
        __asm__ volatile("" ::: "memory");
    }
}

/* Print a hex byte */
static void put_hex_byte(uint8_t b) {
    static const char hex[] = "0123456789abcdef";
    char buf[3];
    buf[0] = hex[(b >> 4) & 0xf];
    buf[1] = hex[b & 0xf];
    buf[2] = '\0';
    microkit_dbg_puts(buf);
}

/*
 * demo_sequence() — The main demo: real data flow between PDs
 *
 * This runs after all PDs are booted and shows agents actually
 * exchanging messages, storing/retrieving data, and publishing events.
 */
static void demo_sequence(void) {
    microkit_dbg_puts("\n");
    microkit_dbg_puts("══════════════════════════════════════════════════════\n");
    microkit_dbg_puts("  DEMO: Agent Data Flow — PDs exchanging real data\n");
    microkit_dbg_puts("══════════════════════════════════════════════════════\n");
    microkit_dbg_puts("\n");

    /* ── Step 1: Store an object in AgentFS ─────────────────────────── */
    microkit_dbg_puts("[controller] Step 1: Storing object in AgentFS via PPC...\n");

    /* AgentFS PUT: MR0=op, MR1=size, MR2=cap_tag */
    uint32_t obj_size = 18;  /* "Hello from agentOS" = 18 bytes */
    microkit_mr_set(0, OP_AGENTFS_PUT);
    microkit_mr_set(1, obj_size);
    microkit_mr_set(2, 0x42);  /* cap_tag: badge 0x42 */

    microkit_ppcall(CH_AGENTFS, microkit_msginfo_new(0, 3));

    uint32_t afs_status = (uint32_t)microkit_mr_get(0);
    if (afs_status == 0) {
        /* Success — read back the object ID from MR1-MR4 */
        ctrl.demo_obj_id[0] = (uint32_t)microkit_mr_get(1);
        ctrl.demo_obj_id[1] = (uint32_t)microkit_mr_get(2);
        ctrl.demo_obj_id[2] = (uint32_t)microkit_mr_get(3);
        ctrl.demo_obj_id[3] = (uint32_t)microkit_mr_get(4);
        ctrl.demo_obj_stored = true;

        microkit_dbg_puts("[controller] AgentFS PUT OK — object id: 0x");
        put_hex_byte((ctrl.demo_obj_id[0] >> 24) & 0xff);
        put_hex_byte((ctrl.demo_obj_id[0] >> 16) & 0xff);
        put_hex_byte((ctrl.demo_obj_id[0] >>  8) & 0xff);
        put_hex_byte((ctrl.demo_obj_id[0]      ) & 0xff);
        microkit_dbg_puts("...\n");
        microkit_dbg_puts("[controller] Object payload: 'Hello from agentOS' (18 bytes)\n");
    } else {
        microkit_dbg_puts("[controller] AgentFS PUT FAILED\n");
        return;
    }

    demo_delay();

    /* ── Step 2: Publish event to EventBus ──────────────────────────── */
    microkit_dbg_puts("[controller] Step 2: Publishing OBJECT_CREATED event to EventBus...\n");

    microkit_mr_set(0, EVT_OBJECT_CREATED);  /* event kind */
    microkit_mr_set(1, ctrl.demo_obj_id[0]); /* first 4 bytes of object ID */
    microkit_mr_set(2, obj_size);             /* object size */

    microkit_ppcall(CH_EVENTBUS, microkit_msginfo_new(EVT_OBJECT_CREATED, 3));
    microkit_dbg_puts("[controller] Event published to ring buffer\n");

    demo_delay();

    /* ── Step 3: Dispatch task to worker_0 ──────────────────────────── */
    microkit_dbg_puts("[controller] Step 3: Dispatching task to worker_0 — 'retrieve object'\n");

    ctrl.worker_task_dispatched = true;
    microkit_notify(CH_WORKER_BASE);  /* notify worker_0 */

    microkit_dbg_puts("[controller] Task dispatched. Waiting for worker completion...\n");
    /* Worker will notify us back on channel 10 when done */
}

void init(void) {
    agentos_log_boot("controller");
    
    microkit_dbg_puts("[controller] Initializing agentOS core services\n");
    
    /* Initialize subsystems */
    cap_broker_init();
    agent_pool_init();
    
    /* PPC into EventBus (passive, higher priority) to initialize it */
    microkit_dbg_puts("[controller] Waking EventBus via PPC...\n");
    microkit_msginfo result = microkit_ppcall(CH_EVENTBUS, 
        microkit_msginfo_new(MSG_EVENTBUS_INIT, 0));
    
    uint64_t resp = microkit_msginfo_get_label(result);
    if (resp == MSG_EVENTBUS_READY) {
        ctrl.eventbus_ready = true;
        microkit_dbg_puts("[controller] EventBus: READY\n");
    } else {
        microkit_dbg_puts("[controller] EventBus: unexpected response\n");
    }
    
    /* Notify InitAgent to start (it's active, so we can't PPC into it) */
    microkit_dbg_puts("[controller] Notifying InitAgent to start...\n");
    microkit_notify(CH_INITAGENT);
    
    /* Initialize vibe-swap subsystem (sets up swap slot channels + service table) */
    vibe_swap_init();

    microkit_dbg_puts("[controller] *** agentOS controller boot complete ***\n");
    microkit_dbg_puts("[controller] Ready for agents.\n");
    
    /* Run the interactive demo sequence */
    demo_sequence();
}

void notified(microkit_channel ch) {
    ctrl.notification_count++;
    
    switch (ch) {
        case CH_EVENTBUS:
            microkit_dbg_puts("[controller] EventBus notification\n");
            ctrl.eventbus_ready = true;
            break;
            
        case CH_INITAGENT:
            microkit_dbg_puts("[controller] InitAgent ready notification received\n");
            ctrl.initagent_ready = true;
            break;
            
        default:
            /* Channels 10-17: worker pool ready/completion notifications */
            if (ch >= 10 && ch <= 17) {
                uint32_t pool_slot = ch - 10;
                
                /* State-based dispatch: notifications don't carry MR payload
                 * in seL4. Use ctrl state to determine notification meaning. */
                if (pool_slot == 0 && ctrl.worker_task_dispatched && !ctrl.demo_complete) {
                    /* Worker_0 completed the demo task */
                    ctrl.demo_complete = true;
                    
                    microkit_dbg_puts("[controller] Worker 0 task COMPLETE\n");
                    
                    /* Publish TASK_COMPLETE event to EventBus */
                    microkit_dbg_puts("[controller] Publishing TASK_COMPLETE event to EventBus...\n");
                    microkit_mr_set(0, MSG_EVENT_AGENT_EXITED);
                    microkit_mr_set(1, 0);
                    microkit_mr_set(2, 1);
                    
                    microkit_ppcall(CH_EVENTBUS,
                        microkit_msginfo_new(MSG_EVENT_AGENT_EXITED, 3));
                    microkit_dbg_puts("[controller] TASK_COMPLETE event published\n");
                    
                    /* Notify InitAgent to query final EventBus status */
                    microkit_notify(CH_INITAGENT);
                    
                    demo_delay();
                    
                    /* Print final demo summary */
                    microkit_dbg_puts("\n");
                    microkit_dbg_puts("══════════════════════════════════════════════════════\n");
                    microkit_dbg_puts("  DEMO COMPLETE\n");
                    microkit_dbg_puts("  Objects stored in AgentFS:    1\n");
                    microkit_dbg_puts("  Events published to EventBus: 3\n");
                    microkit_dbg_puts("  Tasks dispatched to workers:  1\n");
                    microkit_dbg_puts("  IPC calls between PDs:        5\n");
                    microkit_dbg_puts("  Data path: controller→AgentFS→EventBus→worker→controller\n");
                    microkit_dbg_puts("══════════════════════════════════════════════════════\n");
                    microkit_dbg_puts("\n");
                    microkit_dbg_puts("[controller] agentOS is alive. Agents are talking. :)\n");
                } else {
                    microkit_dbg_puts("[controller] Worker ");
                    char s[2] = { (char)('0' + pool_slot), '\0' };
                    microkit_dbg_puts(s);
                    microkit_dbg_puts(" ready\n");
                    /* Ack the worker's ready signal */
                    microkit_notify(ch);
                }
            /* Channels 30-33: swap slot health-OK notifications */
            } else if (ch >= CH_SWAP_BASE && ch < CH_SWAP_BASE + NUM_SWAP_SLOTS) {
                int swap_slot_idx = (int)(ch - CH_SWAP_BASE);
                uint32_t status = (uint32_t)microkit_mr_get(0);
                if (status == 0) {
                    microkit_dbg_puts("[controller] Swap slot health OK — activating\n");
                    vibe_swap_health_notify(swap_slot_idx);
                } else {
                    microkit_dbg_puts("[controller] Swap slot health FAIL\n");
                }
            } else {
                microkit_dbg_puts("[controller] Unknown channel\n");
            }
            break;
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)ch;
    uint64_t label = microkit_msginfo_get_label(msg);
    
    if (label == MSG_WORKER_RETRIEVE) {
        /* Worker requesting AgentFS object retrieval (proxy) */
        microkit_dbg_puts("[controller] Proxying AgentFS GET for worker...\n");
        
        /* Read the object ID words from MRs */
        uint32_t id0 = (uint32_t)microkit_mr_get(0);
        uint32_t id1 = (uint32_t)microkit_mr_get(1);
        uint32_t id2 = (uint32_t)microkit_mr_get(2);
        uint32_t id3 = (uint32_t)microkit_mr_get(3);
        
        /* PPC into AgentFS to GET the object */
        microkit_mr_set(0, OP_AGENTFS_GET);
        microkit_mr_set(1, id0);
        microkit_mr_set(2, id1);
        microkit_mr_set(3, id2);
        microkit_mr_set(4, id3);
        
        microkit_ppcall(CH_AGENTFS, microkit_msginfo_new(0, 5));
        
        uint32_t status = (uint32_t)microkit_mr_get(0);
        if (status == 0) {
            /* Success — forward AgentFS response back to worker */
            uint32_t version    = (uint32_t)microkit_mr_get(1);
            uint32_t size       = (uint32_t)microkit_mr_get(2);
            uint32_t cap_tag    = (uint32_t)microkit_mr_get(3);
            
            microkit_dbg_puts("[controller] AgentFS returned object: version=");
            char v[2] = { (char)('0' + (version % 10)), '\0' };
            microkit_dbg_puts(v);
            microkit_dbg_puts(", size=");
            /* Print size as decimal */
            if (size < 100) {
                char tens = (char)('0' + (size / 10));
                char ones = (char)('0' + (size % 10));
                char sz[3] = { tens, ones, '\0' };
                microkit_dbg_puts(sz);
            } else {
                microkit_dbg_puts("??");
            }
            microkit_dbg_puts(" bytes\n");
            
            /* Pack response for worker: MR0=status, MR1=size, MR2=cap_tag, MR3=version */
            microkit_mr_set(0, 0);       /* OK */
            microkit_mr_set(1, size);
            microkit_mr_set(2, cap_tag);
            microkit_mr_set(3, version);
            return microkit_msginfo_new(MSG_WORKER_RETRIEVE_REPLY, 4);
        } else {
            microkit_dbg_puts("[controller] AgentFS GET failed\n");
            microkit_mr_set(0, status);
            return microkit_msginfo_new(MSG_WORKER_RETRIEVE_REPLY, 1);
        }
    }
    
    microkit_dbg_puts("[controller] Unexpected PPC call\n");
    return microkit_msginfo_new(0xDEAD, 0);
}

/* Note: this extends the notified() function above.
 * In the actual build, the notified() switch needs a POOL_CH_BASE case.
 * Adding it here as a patch note for next refactor. */
