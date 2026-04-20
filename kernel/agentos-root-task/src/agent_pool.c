/*
 * agentOS Agent Pool — Dynamic Agent Execution via Static PDs
 *
 * Design credit: Natasha (correctly identified this beats a hypervisor layer)
 *
 * Problem: Microkit is statically configured — you can't create new PDs
 * at runtime. But we need to spawn agents dynamically.
 *
 * Solution: Pre-allocate N worker PDs in the .system file. Each worker
 * starts in IDLE state, waiting for a task assignment from the controller.
 * Assignment delivers a task_id, a derived capability set, and event
 * channel endpoints. The worker executes the task, publishes results,
 * then returns to IDLE. The controller revokes the task's capabilities.
 *
 * This keeps us entirely within seL4's verified TCB — no dynamic PD
 * creation, no hypervisor layer, no departures from the formal proof.
 *
 * Agent identity is a badge on the endpoint cap, not a PD identity.
 * The PD is just compute substrate; the agent IS its capability set.
 *
 * Pool sizing: start with AGENT_POOL_SIZE=8 workers.
 * Each worker is a separate PD in agentos.system.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/agent_pool_contract.h"
#include "string_bare.h"
#include <stdint.h>

#define AGENT_POOL_SIZE    8
#define TASK_NAME_MAX      64
#define TASK_PAYLOAD_MAX   256

/* Assignment channel IDs (from controller's perspective) */
/* workers are on channels 10..17 (controller end: id=10..17) */
#define POOL_CH_BASE  10

/* Worker states */
typedef enum {
    WORKER_IDLE    = 0,
    WORKER_RUNNING = 1,
    WORKER_DONE    = 2,
    WORKER_FAULT   = 3,
} worker_state_t;

/* A task assignment — what the controller sends to a worker */
typedef struct __attribute__((packed)) {
    uint64_t task_id;                    /* unique task identifier */
    char     agent_name[TASK_NAME_MAX];  /* logical agent name */
    uint32_t cap_count;                  /* number of caps being granted */
    uint32_t priority;                   /* scheduling priority for this task */
    uint8_t  payload[TASK_PAYLOAD_MAX];  /* task-specific config */
    uint32_t payload_len;
    uint32_t timeout_ms;                 /* 0 = no timeout */
} task_assignment_t;

/* A task result — what the worker sends back */
typedef struct __attribute__((packed)) {
    uint64_t task_id;
    int32_t  status;
    uint8_t  result[TASK_PAYLOAD_MAX];
    uint32_t result_len;
    uint64_t cpu_time_us;               /* approximate CPU time used */
} task_result_t;

/* Pool tracking (controller-side) */
typedef struct {
    worker_state_t state;
    uint64_t       task_id;
    uint32_t       channel_id;          /* controller channel to this worker */
    uint64_t       assigned_at_seq;
    char           agent_name[TASK_NAME_MAX];
} pool_slot_t;

static pool_slot_t pool[AGENT_POOL_SIZE];
static uint64_t    task_seq = 0;

/* =========================================================================
 * Controller-side pool management
 * =========================================================================*/

void agent_pool_init(void) {
    for (int i = 0; i < AGENT_POOL_SIZE; i++) {
        pool[i].state      = WORKER_IDLE;
        pool[i].task_id    = 0;
        pool[i].channel_id = POOL_CH_BASE + i;
        pool[i].agent_name[0] = '\0';
    }
    log_drain_write(6, 6, "[pool] Agent pool initialized (");
    /* print pool size */
    char n[4] = { '0' + AGENT_POOL_SIZE, ' ', 'w', '\0' };
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = n; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "orkers)\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(6, 6, _cl_buf);
    }
}

/* Find an idle worker slot */
static int pool_find_idle(void) {
    for (int i = 0; i < AGENT_POOL_SIZE; i++) {
        if (pool[i].state == WORKER_IDLE) return i;
    }
    return -1;
}

/*
 * Spawn an agent task onto an idle worker.
 *
 * Packs the task_assignment_t into message registers and notifies
 * the worker via its channel. The worker wakes up, reads the assignment,
 * and begins executing.
 *
 * Capability delegation happens through seL4 CSpace operations that
 * the controller performs before calling this function — by the time
 * the worker wakes, its capability slots already contain the delegated caps.
 */
int agent_pool_spawn(const char *agent_name, uint64_t task_id,
                      const uint8_t *payload, uint32_t payload_len,
                      uint32_t priority) {
    int slot = pool_find_idle();
    if (slot < 0) {
        log_drain_write(6, 6, "[pool] ERROR: all workers busy\n");
        return -1;
    }
    
    pool[slot].state          = WORKER_RUNNING;
    pool[slot].task_id        = task_id ? task_id : ++task_seq;
    pool[slot].assigned_at_seq = task_seq;
    strncpy(pool[slot].agent_name, agent_name, TASK_NAME_MAX - 1);
    
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = "[pool] Spawning agent '"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = agent_name; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "' on worker slot "; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(6, 6, _cl_buf);
    }
    /* print slot number */
    char s[4] = { '0' + (char)slot, '\n', '\0' };
    log_drain_write(6, 6, s);
    
    /*
     * Pack assignment into MRs for the notification.
     * Workers receive a notification, then read these MRs.
     * MR0: task_id low32    MR1: task_id high32
     * MR2: payload ptr      MR3: payload_len
     *
     * In production: use shared memory for larger payloads.
     */
    microkit_mr_set(0, (uint32_t)(pool[slot].task_id & 0xFFFFFFFF));
    microkit_mr_set(1, (uint32_t)(pool[slot].task_id >> 32));
    microkit_mr_set(2, (uintptr_t)payload);
    microkit_mr_set(3, payload_len);
    
    /* Notify the worker to wake up */
    microkit_notify(pool[slot].channel_id);
    
    return slot;
}

/* Mark a worker done (called from notified() when worker signals completion) */
void agent_pool_worker_done(int slot, int status) {
    if (slot < 0 || slot >= AGENT_POOL_SIZE) return;
    
    log_drain_write(6, 6, "[pool] Worker ");
    char s[4] = { '0' + (char)slot, ' ', 'd', '\0' };
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = s; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "one\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(6, 6, _cl_buf);
    }
    
    /* Revoke the capabilities we delegated to this worker */
    /* In production: iterate cap_grant_log and seL4_CNode_Revoke each cap */
    
    pool[slot].state   = WORKER_IDLE;
    pool[slot].task_id = 0;
    pool[slot].agent_name[0] = '\0';
}

/* Return pool status for monitoring */
void agent_pool_status(int *idle_out, int *running_out) {
    int idle = 0, running = 0;
    for (int i = 0; i < AGENT_POOL_SIZE; i++) {
        if (pool[i].state == WORKER_IDLE)    idle++;
        if (pool[i].state == WORKER_RUNNING) running++;
    }
    if (idle_out)    *idle_out    = idle;
    if (running_out) *running_out = running;
}

/* =========================================================================
 * Worker PD code — runs in each worker_N protection domain
 * =========================================================================*/

/*
 * Worker state (per-PD global, one instance per worker PD)
 */
static struct {
    bool     initialized;
    uint64_t task_id;
    char     agent_name[TASK_NAME_MAX];
    int      my_slot;              /* which pool slot am I? set by init */
} worker = { .initialized = false };

/* Channel IDs from worker's perspective */
#define WORKER_CH_CONTROLLER  0   /* id=0 in worker<->controller channel */
#define WORKER_CH_EVENTBUS    1   /* id=1 in worker<->event_bus channel */

/*
 * worker_init() — called once when the worker PD boots
 * Identifies itself to the pool and enters IDLE state.
 *
 * In the .system file, each worker_N PD has:
 *   setvar_vaddr="worker_slot_id" → patched to its pool index
 */
uintptr_t worker_slot_id = 0;  /* patched by Microkit from .system file */

void worker_pd_init(void) {
    worker.my_slot = (int)worker_slot_id;
    worker.initialized = true;
    
    log_drain_write(6, 6, "[worker] Slot ready, waiting for task assignment\n");
    
    /* Notify controller we're ready */
    microkit_notify(WORKER_CH_CONTROLLER);
}

/*
 * worker_pd_notified() — called when controller sends us a task
 *
 * Reads the task assignment from message registers,
 * executes the task (via a jump table), then notifies completion.
 */
void worker_pd_notified(microkit_channel ch) {
    switch (ch) {
        case WORKER_CH_CONTROLLER: {
            /* Read task assignment from MRs */
            uint64_t task_id_lo = (uint64_t)microkit_mr_get(0);
            uint64_t task_id_hi = (uint64_t)microkit_mr_get(1);
            uint64_t payload_ptr = (uint64_t)microkit_mr_get(2);
            uint32_t payload_len = (uint32_t)microkit_mr_get(3);
            
            worker.task_id = task_id_lo | (task_id_hi << 32);
            
            log_drain_write(6, 6, "[worker] Task assigned\n");
            
            /* 
             * Execute the task.
             *
             * In the full agentOS design, the "task" is a function pointer
             * resolved from a task registry maintained by the controller.
             * For v0.1: we just acknowledge and return.
             *
             * In Phase 2: workers will load a WASM module via MemFS,
             * execute it in a sandboxed interpreter (wasm3 or wamr),
             * and deliver results via EventBus.
             */
            log_drain_write(6, 6, "[worker] Task running...\n");
            
            /* TODO Phase 2: WASM module execution */
            int status = 0;
            
            log_drain_write(6, 6, "[worker] Task complete, notifying controller\n");
            
            /* Signal completion back to controller */
            microkit_mr_set(0, (uint32_t)(worker.task_id & 0xFFFFFFFF));
            microkit_mr_set(1, (uint32_t)(worker.task_id >> 32));
            microkit_mr_set(2, (uint32_t)status);
            microkit_mr_set(3, 0);
            microkit_notify(WORKER_CH_CONTROLLER);
            break;
        }
        
        case WORKER_CH_EVENTBUS:
            /* EventBus notification — process incoming events if subscribed */
            log_drain_write(6, 6, "[worker] EventBus notification\n");
            break;
            
        default:
            log_drain_write(6, 6, "[worker] Unknown channel notification\n");
            break;
    }
}
