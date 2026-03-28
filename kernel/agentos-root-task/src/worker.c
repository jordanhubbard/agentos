/*
 * agentOS Worker PD
 *
 * One binary, N instances. Each instance is a separate protection domain
 * in the .system file with a different worker_slot_id.
 *
 * Lifecycle:
 *   boot → report_ready → IDLE (seL4_Recv) → task_assigned → run → done → IDLE
 *
 * The worker's capability set is replenished per task by the controller.
 * Between tasks, it holds only its own stack and the two channel endpoints.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>
#include "string_bare.h"

/* Patched by Microkit at image build time from .system setvar */
uintptr_t worker_slot_id = 0;

/* Worker channels */
#define CH_CONTROLLER 0
#define CH_EVENTBUS   1

/* Worker state */
static struct {
    uint64_t current_task_id;
    uint32_t tasks_completed;
    bool     running;
} wstate = { 0, 0, false };

void init(void) {
    microkit_dbg_puts("[worker] Slot ");
    /* Print slot id - microkit_dbg_puts needs a null-terminated string */
    char slot_str[4] = { (char)('0' + (worker_slot_id & 0xF)), ':', ' ', '\0' };
    microkit_dbg_puts(slot_str);
    microkit_dbg_puts("ready, notifying controller\n");
    
    /* Signal controller: we're ready for work */
    microkit_notify(CH_CONTROLLER);
}

void notified(microkit_channel ch) {
    if (ch == CH_CONTROLLER) {
        /* Controller is sending us a task or acknowledging our completion */
        uint64_t task_id_lo = (uint64_t)microkit_mr_get(0);
        uint64_t task_id_hi = (uint64_t)microkit_mr_get(1);
        uint64_t task_id    = task_id_lo | (task_id_hi << 32);
        
        if (task_id > 0) {
            /* New task assignment */
            wstate.current_task_id = task_id;
            wstate.running = true;
            
            microkit_dbg_puts("[worker] Task received\n");
            
            /*
             * Execute the task.
             *
             * Phase 1 (now): acknowledge and return.
             * Phase 2: load WASM from MemFS, execute in wasm3 sandbox.
             * Phase 3: full agent runtime with EventBus pub/sub.
             */
            
            /* Simulate work */
            /* (In production: actual task execution here) */
            
            wstate.tasks_completed++;
            wstate.running = false;
            
            /* Report completion back to controller */
            microkit_mr_set(0, (uint32_t)(task_id & 0xFFFFFFFF));
            microkit_mr_set(1, (uint32_t)(task_id >> 32));
            microkit_mr_set(2, 0); /* status: OK */
            microkit_mr_set(3, wstate.tasks_completed);
            microkit_notify(CH_CONTROLLER);
            
        } else {
            /* task_id == 0: controller acknowledging our ready signal */
            microkit_dbg_puts("[worker] Acknowledged by controller\n");
        }
        
    } else if (ch == CH_EVENTBUS) {
        /* EventBus event notification */
        if (wstate.running) {
            /* Forward to running task context */
            microkit_dbg_puts("[worker] EventBus notification during task\n");
        }
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    /* Workers don't accept PPC from external callers in v0.1 */
    (void)ch; (void)msg;
    return microkit_msginfo_new(0xDEAD, 0);
}
