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

/* Forward declaration */
void vibe_swap_init(void);
int  vibe_swap_health_notify(int slot);

/* Controller state */
static struct {
    bool eventbus_ready;
    bool initagent_ready;
    uint32_t notification_count;
} ctrl = { false, false, 0 };

void init(void) {
    agentos_log_boot("controller");
    
    microkit_dbg_puts("[controller] Initializing agentOS core services\n");
    
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
                (void)pool_slot;
                uint64_t task_id = (uint64_t)microkit_mr_get(0) |
                                   ((uint64_t)microkit_mr_get(1) << 32);
                if (task_id == 0) {
                    microkit_dbg_puts("[controller] Worker ready\n");
                } else {
                    microkit_dbg_puts("[controller] Worker task complete\n");
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
    /* Controller is not passive - this shouldn't be called */
    (void)ch; (void)msg;
    microkit_dbg_puts("[controller] Unexpected PPC call\n");
    return microkit_msginfo_new(0xDEAD, 0);
}

/* Note: this extends the notified() function above.
 * In the actual build, the notified() switch needs a POOL_CH_BASE case.
 * Adding it here as a patch note for next refactor. */
