/*
 * agentOS Controller Protection Domain
 * 
 * Priority 50. Lowest of the three PDs so it can PPC into higher-priority servers.
 * Controls the EventBus (passive, prio 200) and coordinates with InitAgent (prio 100).
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>

/* Memory region - patched by Microkit */
uintptr_t monitor_stack_vaddr;

/* Channel IDs (must match agentos.system id= values) */
#define CH_EVENTBUS   0
#define CH_INITAGENT  1

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
            microkit_dbg_puts("[controller] Unknown channel notification\n");
            break;
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    /* Controller is not passive - this shouldn't be called */
    (void)ch; (void)msg;
    microkit_dbg_puts("[controller] Unexpected PPC call\n");
    return microkit_msginfo_new(0xDEAD, 0);
}
