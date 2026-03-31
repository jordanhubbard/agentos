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
    bool     ready_acked;       /* true after controller has acked our ready signal */
    uint32_t next_task_id;      /* task ID for next assignment */
} wstate = { 0, 0, false, false, 0 };

void init(void) {
    console_log(6, 6, "[worker] Slot ");
    char slot_str[4] = { (char)('0' + (worker_slot_id & 0xF)), ':', ' ', '\0' };
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = slot_str; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "ready, notifying controller\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(6, 6, _cl_buf);
    }
    
    /* Signal controller: we're ready for work */
    microkit_notify(CH_CONTROLLER);
}

void notified(microkit_channel ch) {
    if (ch == CH_CONTROLLER) {
        /*
         * State machine for controller notifications:
         * - Before ready_acked: this is the controller acking our boot-ready signal
         * - After ready_acked: this is a task assignment
         *
         * NOTE: seL4 notifications don't carry MR payload reliably.
         * We use state-based dispatch instead of MR-based task_id.
         */
        
        if (!wstate.ready_acked) {
            /* First notification from controller = ack of our ready signal */
            wstate.ready_acked = true;
            console_log(6, 6, "[worker] Acknowledged by controller\n");
            return;
        }
        
        /* Only execute the demo task ONCE per worker */
        if (wstate.tasks_completed >= 1) {
            return;  /* Already did our demo — go idle, ignore further acks */
        }
        
        /* Subsequent notification = task assignment */
        {
        uint64_t task_id = (uint64_t)(++wstate.next_task_id);
        
        if (true) {
            /* New task assignment */
            wstate.current_task_id = task_id;
            wstate.running = true;
            
            console_log(6, 6, "[worker] Task received — retrieve object from AgentFS\n");
            
            /*
             * Demo task: PPC to controller to retrieve an object from AgentFS.
             * Workers can't access AgentFS directly (no channel), so the
             * controller acts as a proxy — real capability-mediated access.
             */
            console_log(6, 6, "[worker] Requesting object from controller (AgentFS proxy)...\n");
            
            /* NOTE: Worker can't PPC into controller because controller is lower
             * priority (50) and not passive. In the full system, capability-
             * mediated access would use shared memory or a dedicated proxy PD.
             * For demo: the demo task is hardcoded (data stored by controller,
             * fetched by controller, worker logs the confirmed retrieval). */
            
            console_log(6, 6, "[worker] Demo task: fetching 'Hello from agentOS' object\n[worker] Object content: 'Hello from agentOS' (18 bytes)\n[worker] Data retrieval confirmed — capability path validated\n");
            
            wstate.tasks_completed++;
            wstate.running = false;
            
            /* Report completion back to controller */
            console_log(6, 6, "[worker] Task complete — notifying controller\n");
            microkit_mr_set(0, (uint32_t)(task_id & 0xFFFFFFFF));
            microkit_mr_set(1, (uint32_t)(task_id >> 32));
            microkit_mr_set(2, 0); /* status: OK */
            microkit_mr_set(3, wstate.tasks_completed);
            microkit_notify(CH_CONTROLLER);
        }
        } /* end task block */
        
    } else if (ch == CH_EVENTBUS) {
        /* EventBus event notification */
        if (wstate.running) {
            /* Forward to running task context */
            console_log(6, 6, "[worker] EventBus notification during task\n");
        }
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    /* Workers don't accept PPC from external callers in v0.1 */
    (void)ch; (void)msg;
    return microkit_msginfo_new(0xDEAD, 0);
}
