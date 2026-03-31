/*
 * agentOS InitAgent Protection Domain
 * 
 * Priority 100. First real agent. Bootstraps the agent ecosystem.
 * Receives start notification from controller, subscribes to EventBus,
 * prints the boot banner.
 *
 * Phase 2: Also serves as the dynamic spawn broker.
 * Any PD with a channel to init_agent may PPC MSG_SPAWN_AGENT to request
 * a new agent be loaded from AgentFS into a worker pool slot.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>

/* Channel IDs (from init_agent's perspective, matching agentos.system) */
#define CH_CONTROLLER 1   /* id="1" in controller<->initagent channel, init_agent end */
#define CH_EVENTBUS   2   /* id="2" in eventbus<->initagent channel, init_agent end */
#define CH_QUOTA      7   /* id="7" in initagent<->quota_pd channel, init_agent end */
#define CH_MEM_PROF   8   /* id="8" in initagent<->mem_profiler channel, init_agent end */

/* Default quota limits for spawned agents */
#define DEFAULT_CPU_QUOTA_MS   5000   /* 5 seconds CPU time */
#define DEFAULT_MEM_QUOTA_KB   4096   /* 4MB memory */

/* ── Spawn broker ─────────────────────────────────────────────────────────
 *
 * init_agent is the single spawn broker. Callers PPC MSG_SPAWN_AGENT:
 *   MR0: wasm_hash_lo  (low  64 bits of BLAKE3 hash)
 *   MR1: wasm_hash_hi  (high 64 bits of BLAKE3 hash)
 *   MR2: priority      (suggested scheduling priority for new agent)
 *   MR3: flags         (reserved, pass 0)
 *
 * init_agent enqueues the request, notifies the controller (which holds
 * the pool and channels to all worker PDs), then returns immediately with:
 *   MR0: spawn_id      (provisional request ID, PENDING_FLAG set if async)
 *   MR1: status        (0 = accepted/queued, non-zero = error)
 *
 * When the controller completes the assignment it notifies init_agent on
 * CH_CONTROLLER with MR0=MSG_SPAWN_AGENT_REPLY, MR1=spawn_id, MR2=slot_id.
 * init_agent updates the table and publishes an EVT_AGENT_SPAWNED event.
 *
 * Controller spawn notification MR layout (init_agent → controller):
 *   MR0: MSG_SPAWN_AGENT (tag)
 *   MR1: wasm_hash_lo
 *   MR2: wasm_hash_hi
 *   MR3: priority
 *   MR4: spawn_id (so controller can correlate replies)
 */

#define MAX_PENDING_SPAWNS  8
#define SPAWN_PENDING_FLAG  0x80000000u   /* set in spawn_id when still pending */

typedef struct {
    uint32_t spawn_id;       /* request ID — lower 16 bits of spawn_seq */
    uint64_t hash_lo;
    uint64_t hash_hi;
    uint32_t priority;
    bool     pending;        /* true until controller confirms slot */
    int32_t  slot_id;        /* -1 until assigned */
    bool     quota_registered;
    uint32_t quota_cpu_ms;
    uint32_t quota_mem_kb;
} spawn_req_t;

static spawn_req_t spawn_table[MAX_PENDING_SPAWNS];
static uint32_t    spawn_seq = 0;

static void spawn_table_init(void) {
    for (int i = 0; i < MAX_PENDING_SPAWNS; i++) {
        spawn_table[i].pending = false;
        spawn_table[i].spawn_id = 0;
        spawn_table[i].slot_id = -1;
        spawn_table[i].quota_registered = false;
        spawn_table[i].quota_cpu_ms = 0;
        spawn_table[i].quota_mem_kb = 0;
    }
}

/* Find an empty slot in the pending table */
static int spawn_table_alloc(void) {
    for (int i = 0; i < MAX_PENDING_SPAWNS; i++) {
        if (!spawn_table[i].pending) return i;
    }
    return -1;
}

/* Lookup by spawn_id */
static int spawn_table_find(uint32_t spawn_id) {
    for (int i = 0; i < MAX_PENDING_SPAWNS; i++) {
        if (spawn_table[i].pending && spawn_table[i].spawn_id == spawn_id) return i;
    }
    return -1;
}

/* ── Init/Status state ────────────────────────────────────────────────── */

static struct {
    bool started;
    bool eventbus_subscribed;
    uint32_t event_count;
    uint32_t query_count;
    uint32_t spawn_count;     /* total agents spawned */
} state = { false, false, 0, 0, 0 };

/* ── Banner ───────────────────────────────────────────────────────────── */

static void print_banner(void) {
    microkit_dbg_puts("\n");
    microkit_dbg_puts("╔══════════════════════════════════════════════════╗\n");
    microkit_dbg_puts("║                                                  ║\n");
    microkit_dbg_puts("║          agentOS v0.1.0-alpha                    ║\n");
    microkit_dbg_puts("║   The World's First OS for AI Agents             ║\n");
    microkit_dbg_puts("║                                                  ║\n");
    microkit_dbg_puts("║   Built on: seL4 Microkernel (formally proved)   ║\n");
#if defined(__aarch64__)
    microkit_dbg_puts("║   Arch:     seL4 Microkit / AArch64              ║\n");
#elif defined(__riscv)
    microkit_dbg_puts("║   Arch:     seL4 Microkit / RISC-V RV64          ║\n");
#else
    microkit_dbg_puts("║   Arch:     seL4 Microkit                        ║\n");
#endif
    microkit_dbg_puts("║                                                  ║\n");
    microkit_dbg_puts("║   Protection Domains:                            ║\n");
    microkit_dbg_puts("║     [*] controller  (prio  50) - system ctrl     ║\n");
    microkit_dbg_puts("║     [*] event_bus   (prio 200) - pub/sub bus     ║\n");
    microkit_dbg_puts("║     [*] init_agent  (prio 100) - bootstrapper    ║\n");
    microkit_dbg_puts("║                                                  ║\n");
    microkit_dbg_puts("║   Ready for agents. The future is running.       ║\n");
    microkit_dbg_puts("║                                                  ║\n");
    microkit_dbg_puts("║   Designed by Natasha on 2026-03-28              ║\n");
    microkit_dbg_puts("║   github.com/jordanhubbard/agentos               ║\n");
    microkit_dbg_puts("╚══════════════════════════════════════════════════╝\n");
    microkit_dbg_puts("\n");
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void put_dec(uint32_t v) {
    if (v == 0) { microkit_dbg_puts("0"); return; }
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    }
    microkit_dbg_puts(&buf[i]);
}

static void put_hex32(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[11] = "0x00000000";
    for (int i = 9; i >= 2; i--) {
        buf[i] = hex[v & 0xf];
        v >>= 4;
    }
    microkit_dbg_puts(buf);
}

/* ── Quota ops ────────────────────────────────────────────────────────── */

/*
 * Register a newly spawned agent with the quota system.
 * Called after successful spawn to enforce resource limits.
 */
static bool quota_register_agent(uint32_t agent_id, uint32_t cpu_ms, uint32_t mem_kb) {
    microkit_mr_set(0, OP_QUOTA_REGISTER);
    microkit_mr_set(1, agent_id);
    microkit_mr_set(2, cpu_ms);
    microkit_mr_set(3, mem_kb);
    microkit_ppcall(CH_QUOTA, microkit_msginfo_new(0, 4));

    uint32_t slot   = (uint32_t)microkit_mr_get(0);
    uint32_t status = (uint32_t)microkit_mr_get(1);

    if (status == 0 || status == 1) {
        microkit_dbg_puts("[init_agent] Quota registered: agent=");
        put_dec(agent_id);
        microkit_dbg_puts(" cpu=");
        put_dec(cpu_ms);
        microkit_dbg_puts("ms mem=");
        put_dec(mem_kb);
        microkit_dbg_puts("kb slot=");
        put_dec(slot);
        microkit_dbg_puts("\n");
        return true;
    }

    microkit_dbg_puts("[init_agent] Quota registration failed for agent=");
    put_dec(agent_id);
    microkit_dbg_puts(" status=");
    put_dec(status);
    microkit_dbg_puts("\n");
    return false;
}

/*
 * Tick an agent's quota usage. Called per scheduler round.
 * Returns the agent's quota flags (check for revocation).
 */
static uint32_t quota_tick_agent(uint32_t agent_id, uint32_t cpu_delta_ms, uint32_t mem_cur_kb) {
    microkit_mr_set(0, OP_QUOTA_TICK);
    microkit_mr_set(1, agent_id);
    microkit_mr_set(2, cpu_delta_ms);
    microkit_mr_set(3, mem_cur_kb);
    microkit_ppcall(CH_QUOTA, microkit_msginfo_new(0, 4));

    uint32_t result = (uint32_t)microkit_mr_get(0);
    if (result != 0) return result;

    return (uint32_t)microkit_mr_get(1);  /* flags */
}

static void quota_tick_all_agents(void) {
    for (int i = 0; i < MAX_PENDING_SPAWNS; i++) {
        if (!spawn_table[i].quota_registered) continue;
        if (spawn_table[i].pending) continue;

        uint32_t flags = quota_tick_agent(spawn_table[i].spawn_id, 1, 0);
        if (flags == 0xE2) {
            /* Agent not found yet — likely not registered */
            continue;
        }

        if (flags & QUOTA_FLAG_REVOKED) {
            microkit_dbg_puts("[init_agent] Quota revoked agent=");
            put_dec(spawn_table[i].spawn_id);
            microkit_dbg_puts(" flags=0x");
            char hexbuf[9];
            for (int h = 0; h < 8; h++) {
                uint32_t nibble = (flags >> (28 - h * 4)) & 0xF;
                hexbuf[h] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
            }
            hexbuf[8] = '\0';
            microkit_dbg_puts(hexbuf);
            microkit_dbg_puts("\n");
            spawn_table[i].quota_registered = false;
        }
    }
}

static void scheduler_round_tick(void) {
    quota_tick_all_agents();
}

/* ── EventBus ops ─────────────────────────────────────────────────────── */

static void query_eventbus_status(void) {
    state.query_count++;

    microkit_dbg_puts("[init_agent] Querying EventBus status via PPC...\n");

    microkit_ppcall(CH_EVENTBUS, microkit_msginfo_new(MSG_EVENTBUS_STATUS, 0));

    uint64_t total_events = (uint64_t)microkit_mr_get(0);
    uint32_t subscribers  = (uint32_t)microkit_mr_get(1);

    microkit_dbg_puts("\n");
    microkit_dbg_puts("[init_agent] ── EventBus Audit Report ───────────────────\n");
    microkit_dbg_puts("[init_agent]   Total events published: ");
    put_dec((uint32_t)total_events);
    microkit_dbg_puts("\n");
    microkit_dbg_puts("[init_agent]   Active subscribers: ");
    put_dec(subscribers);
    microkit_dbg_puts("\n");
    microkit_dbg_puts("[init_agent]   Events since last query: ");
    uint32_t new_events = (uint32_t)total_events - state.event_count;
    put_dec(new_events);
    microkit_dbg_puts("\n");
    microkit_dbg_puts("[init_agent]   Agents spawned this session: ");
    put_dec(state.spawn_count);
    microkit_dbg_puts("\n");

    if (total_events > 0) {
        microkit_dbg_puts("[init_agent]   Data flow confirmed: agents exchanging messages\n");
    }

    microkit_dbg_puts("[init_agent] ────────────────────────────────────────────\n");

    state.event_count = (uint32_t)total_events;
}

static void subscribe_to_eventbus(void) {
    microkit_dbg_puts("[init_agent] Subscribing to EventBus...\n");
    
    microkit_mr_set(0, CH_EVENTBUS);
    microkit_mr_set(1, 0);
    
    microkit_msginfo result = microkit_ppcall(
        CH_EVENTBUS,
        microkit_msginfo_new(MSG_EVENTBUS_SUBSCRIBE, 2)
    );
    
    if (microkit_msginfo_get_label(result) == 0) {
        state.eventbus_subscribed = true;
        microkit_dbg_puts("[init_agent] EventBus subscription: OK\n");
    } else {
        microkit_dbg_puts("[init_agent] EventBus subscription: FAILED\n");
    }
}

/* ── Spawn broker ─────────────────────────────────────────────────────── */

/*
 * Publish EVT_AGENT_SPAWNED to the EventBus ring.
 * slot_id becomes the event source_pd field for routing.
 */
static void publish_spawn_event(uint32_t spawn_id, int32_t slot_id,
                                 uint64_t hash_lo, uint64_t hash_hi) {
    /*
     * Pack spawn notification into MRs for EventBus publish PPC.
     * MR0: MSG_EVENT_PUBLISH
     * MR1: event kind = MSG_EVENT_AGENT_SPAWNED
     * MR2: spawn_id
     * MR3: slot_id
     * MR4: hash_lo (low 32)
     * MR5: hash_hi (low 32)
     */
    microkit_mr_set(0, MSG_EVENT_PUBLISH);
    microkit_mr_set(1, MSG_EVENT_AGENT_SPAWNED);
    microkit_mr_set(2, spawn_id);
    microkit_mr_set(3, (uint32_t)slot_id);
    microkit_mr_set(4, (uint32_t)(hash_lo & 0xFFFFFFFF));
    microkit_mr_set(5, (uint32_t)(hash_hi & 0xFFFFFFFF));
    microkit_ppcall(CH_EVENTBUS, microkit_msginfo_new(MSG_EVENT_PUBLISH, 6));
}

/*
 * Request the controller to assign a worker slot for the WASM hash.
 *
 * Notification layout (init_agent → controller):
 *   MR0: MSG_SPAWN_AGENT (tag for controller's notified() handler)
 *   MR1: wasm_hash_lo
 *   MR2: wasm_hash_hi
 *   MR3: priority
 *   MR4: spawn_id
 */
static void request_controller_spawn(uint32_t spawn_id,
                                      uint64_t hash_lo, uint64_t hash_hi,
                                      uint32_t priority) {
    microkit_mr_set(0, MSG_SPAWN_AGENT);
    microkit_mr_set(1, (uint32_t)(hash_lo & 0xFFFFFFFF));
    microkit_mr_set(2, (uint32_t)((hash_lo >> 32) & 0xFFFFFFFF));
    microkit_mr_set(3, (uint32_t)(hash_hi & 0xFFFFFFFF));
    microkit_mr_set(4, spawn_id);
    microkit_mr_set(5, priority);
    microkit_notify(CH_CONTROLLER);
}

/*
 * Handle MSG_SPAWN_AGENT in the protected() handler.
 * Called synchronously — must return quickly.
 */
static microkit_msginfo handle_spawn_agent(microkit_msginfo msg) {
    (void)msg;

    uint64_t hash_lo  = (uint64_t)microkit_mr_get(0) |
                        ((uint64_t)microkit_mr_get(1) << 32);
    uint64_t hash_hi  = (uint64_t)microkit_mr_get(2) |
                        ((uint64_t)microkit_mr_get(3) << 32);
    uint32_t priority = (uint32_t)microkit_mr_get(4);

    if (priority == 0) priority = PRIO_COMPUTE;  /* sensible default */

    microkit_dbg_puts("[init_agent] SPAWN_AGENT request: hash_lo=");
    put_hex32((uint32_t)(hash_lo & 0xFFFFFFFF));
    microkit_dbg_puts(" priority=");
    put_dec(priority);
    microkit_dbg_puts("\n");

    /* Allocate a pending entry */
    int tbl = spawn_table_alloc();
    if (tbl < 0) {
        microkit_dbg_puts("[init_agent] SPAWN_AGENT: pending table full\n");
        microkit_mr_set(0, 0);
        microkit_mr_set(1, 0xE1);  /* ERR_SPAWN_TABLE_FULL */
        return microkit_msginfo_new(MSG_SPAWN_AGENT_REPLY, 2);
    }

    uint32_t spawn_id = (++spawn_seq) & 0x7FFFFFFFu;
    spawn_table[tbl].spawn_id  = spawn_id;
    spawn_table[tbl].hash_lo   = hash_lo;
    spawn_table[tbl].hash_hi   = hash_hi;
    spawn_table[tbl].priority  = priority;
    spawn_table[tbl].pending   = true;
    spawn_table[tbl].slot_id   = -1;

    /* Ask the controller to actually load the WASM into a worker slot */
    request_controller_spawn(spawn_id, hash_lo, hash_hi, priority);

    microkit_dbg_puts("[init_agent] SPAWN_AGENT queued, spawn_id=");
    put_dec(spawn_id);
    microkit_dbg_puts("\n");

    /*
     * Return provisional spawn_id with PENDING_FLAG.
     * Callers poll MSG_INITAGENT_STATUS or wait for the EVT_AGENT_SPAWNED
     * event on the EventBus to learn the final slot_id.
     */
    microkit_mr_set(0, spawn_id | SPAWN_PENDING_FLAG);
    microkit_mr_set(1, 0);  /* status: OK (queued) */
    return microkit_msginfo_new(MSG_SPAWN_AGENT_REPLY, 2);
}

/*
 * Handle MSG_SPAWN_AGENT_REPLY notification from the controller.
 * Controller sets: MR0=MSG_SPAWN_AGENT_REPLY, MR1=spawn_id, MR2=slot_id
 * (negative slot_id = spawn failed)
 */
static void handle_spawn_reply_from_controller(void) {
    uint32_t tag      = (uint32_t)microkit_mr_get(0);
    uint32_t spawn_id = (uint32_t)microkit_mr_get(1);
    int32_t  slot_id  = (int32_t) microkit_mr_get(2);

    if (tag != MSG_SPAWN_AGENT_REPLY) {
        /* Not a spawn reply — fall through to the regular handler */
        microkit_dbg_puts("[init_agent] Controller notification (non-spawn)\n");
        query_eventbus_status();
        return;
    }

    int tbl = spawn_table_find(spawn_id);
    if (tbl < 0) {
        microkit_dbg_puts("[init_agent] SPAWN_REPLY: unknown spawn_id\n");
        return;
    }

    spawn_table[tbl].slot_id = slot_id;
    spawn_table[tbl].pending = (slot_id < 0);  /* mark done if successful */

    if (slot_id >= 0) {
        state.spawn_count++;
        microkit_dbg_puts("[init_agent] Agent spawned: slot=");
        put_dec((uint32_t)slot_id);
        microkit_dbg_puts(" spawn_id=");
        put_dec(spawn_id);
        microkit_dbg_puts("\n");

        /* Register the new agent with the quota system */
        bool quota_ok = quota_register_agent(spawn_id, DEFAULT_CPU_QUOTA_MS, DEFAULT_MEM_QUOTA_KB);
        spawn_table[tbl].quota_registered = quota_ok;

        /* Register the new agent with the memory profiler */
        microkit_mr_set(0, 0xC0);  /* OP_MEM_REGISTER */
        microkit_mr_set(1, spawn_id);
        microkit_ppcall(CH_MEM_PROF, microkit_msginfo_new(0, 2));
        microkit_dbg_puts("[init_agent] mem_profiler registered slot=");
        put_dec(spawn_id);
        microkit_dbg_puts("\n");
        spawn_table[tbl].quota_cpu_ms     = DEFAULT_CPU_QUOTA_MS;
        spawn_table[tbl].quota_mem_kb     = DEFAULT_MEM_QUOTA_KB;

        /* Publish to EventBus so all subscribers learn about the new agent */
        publish_spawn_event(spawn_id, slot_id,
                            spawn_table[tbl].hash_lo,
                            spawn_table[tbl].hash_hi);

        /* Clear the pending entry */
        spawn_table[tbl].pending = false;
    } else {
        microkit_dbg_puts("[init_agent] SPAWN_REPLY: controller reported failure\n");
        spawn_table[tbl].pending = false;
        spawn_table[tbl].quota_registered = false;
    }
}

/* ── Microkit entry points ────────────────────────────────────────────── */

void init(void) {
    microkit_dbg_puts("[init_agent] Starting up...\n");

    spawn_table_init();
    
    subscribe_to_eventbus();
    
    microkit_dbg_puts("[init_agent] Notifying controller: ready\n");
    microkit_notify(CH_CONTROLLER);
    
    print_banner();
    
    microkit_dbg_puts("[init_agent] Entering event loop. agentOS is ALIVE.\n");
    state.started = true;
}

void notified(microkit_channel ch) {
    switch (ch) {
        case CH_CONTROLLER:
            /*
             * Could be a spawn reply (MR0 = MSG_SPAWN_AGENT_REPLY),
             * a startup trigger, or a general status ping.
             */
            if (!state.started) {
                microkit_dbg_puts("[init_agent] Start signal from controller\n");
            } else {
                handle_spawn_reply_from_controller();
            }
            break;
            
        case CH_EVENTBUS:
            microkit_dbg_puts("[init_agent] EventBus notification\n");
            query_eventbus_status();
            break;
            
        default:
            microkit_dbg_puts("[init_agent] Unknown notification channel\n");
            break;
    }

    scheduler_round_tick();
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)ch;
    uint64_t tag = microkit_msginfo_get_label(msg);
    
    switch (tag) {
        case MSG_INITAGENT_STATUS:
            microkit_mr_set(0, state.event_count);
            microkit_mr_set(1, state.eventbus_subscribed ? 1 : 0);
            microkit_mr_set(2, state.spawn_count);
            return microkit_msginfo_new(0, 3);

        case MSG_SPAWN_AGENT:
            /*
             * Dynamic agent spawn request.
             * MR0/MR1: wasm_hash_lo  (64-bit as two u32 LE)
             * MR2/MR3: wasm_hash_hi  (64-bit as two u32 LE)
             * MR4:     priority      (0 = use PRIO_COMPUTE default)
             */
            return handle_spawn_agent(msg);
            
        default:
            return microkit_msginfo_new(0xFFFF, 0);
    }
}
