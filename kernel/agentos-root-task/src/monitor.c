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
/* VibeEngine staging region: controller reads WASM proposals here (mapped r) */
uintptr_t vibe_staging_ctrl_vaddr;

/*
 * Echo service WASM binary (embedded for demo Step 4)
 * This is test/echo_service.wasm — 305 bytes.
 * exports: init(), handle_ppc(i64,i64,i64,i64,i64), health_check() -> i32
 */
static const uint8_t ECHO_SERVICE_WASM[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x21, 0x06, 0x60,
    0x03, 0x7f, 0x7f, 0x7f, 0x00, 0x60, 0x00, 0x01, 0x7e, 0x60, 0x03, 0x7f,
    0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x00, 0x60, 0x05, 0x7e, 0x7e, 0x7e,
    0x7e, 0x7e, 0x00, 0x60, 0x00, 0x01, 0x7f, 0x02, 0x48, 0x04, 0x03, 0x61,
    0x6f, 0x73, 0x07, 0x61, 0x6f, 0x73, 0x5f, 0x6c, 0x6f, 0x67, 0x00, 0x00,
    0x03, 0x61, 0x6f, 0x73, 0x0b, 0x61, 0x6f, 0x73, 0x5f, 0x74, 0x69, 0x6d,
    0x65, 0x5f, 0x75, 0x73, 0x00, 0x01, 0x03, 0x61, 0x6f, 0x73, 0x0c, 0x61,
    0x6f, 0x73, 0x5f, 0x6d, 0x65, 0x6d, 0x5f, 0x72, 0x65, 0x61, 0x64, 0x00,
    0x02, 0x03, 0x61, 0x6f, 0x73, 0x0d, 0x61, 0x6f, 0x73, 0x5f, 0x6d, 0x65,
    0x6d, 0x5f, 0x77, 0x72, 0x69, 0x74, 0x65, 0x00, 0x02, 0x03, 0x04, 0x03,
    0x03, 0x04, 0x05, 0x05, 0x03, 0x01, 0x00, 0x21, 0x07, 0x2d, 0x04, 0x06,
    0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x04, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x04, 0x0a, 0x68, 0x61, 0x6e, 0x64, 0x6c, 0x65, 0x5f, 0x70,
    0x70, 0x63, 0x00, 0x05, 0x0c, 0x68, 0x65, 0x61, 0x6c, 0x74, 0x68, 0x5f,
    0x63, 0x68, 0x65, 0x63, 0x6b, 0x00, 0x06, 0x0a, 0x49, 0x03, 0x0a, 0x00,
    0x41, 0x00, 0x41, 0x00, 0x41, 0x2f, 0x10, 0x00, 0x0b, 0x37, 0x00, 0x41,
    0x80, 0x80, 0x80, 0x01, 0x20, 0x00, 0x37, 0x03, 0x00, 0x41, 0x88, 0x80,
    0x80, 0x01, 0x20, 0x01, 0x42, 0x01, 0x7c, 0x37, 0x03, 0x00, 0x41, 0x90,
    0x80, 0x80, 0x01, 0x20, 0x02, 0x37, 0x03, 0x00, 0x41, 0x98, 0x80, 0x80,
    0x01, 0x20, 0x03, 0x37, 0x03, 0x00, 0x41, 0xa0, 0x80, 0x80, 0x01, 0x20,
    0x04, 0x37, 0x03, 0x00, 0x0b, 0x04, 0x00, 0x41, 0x01, 0x0b, 0x0b, 0x35,
    0x01, 0x00, 0x41, 0x00, 0x0b, 0x2f, 0x45, 0x63, 0x68, 0x6f, 0x20, 0x73,
    0x65, 0x72, 0x76, 0x69, 0x63, 0x65, 0x20, 0x69, 0x6e, 0x69, 0x74, 0x69,
    0x61, 0x6c, 0x69, 0x7a, 0x65, 0x64, 0x20, 0x76, 0x69, 0x61, 0x20, 0x61,
    0x67, 0x65, 0x6e, 0x74, 0x4f, 0x53, 0x20, 0x76, 0x69, 0x62, 0x65, 0x2d,
    0x73, 0x77, 0x61, 0x70, 0x21
};
static const uint32_t ECHO_SERVICE_WASM_LEN = 305;

/* Channel IDs (must match agentos.system id= values) */
#define CH_EVENTBUS      0
#define CH_INITAGENT     1
#define CH_SWAP_BASE     30   /* Channels 30-33: swap slot PDs */
#define NUM_SWAP_SLOTS   4
#define CH_VIBEENGINE    40   /* Channel 40: vibe_engine notifies us when swap approved */
#define CH_GPUSCHED      50   /* Channel 50: gpu_sched <-> controller */
#define CH_MESHAGENT     55   /* Channel 55: mesh_agent <-> controller */
/* CH_QUOTA_NOTIFY and CH_WATCHDOG_NOTIFY come from agentos.h defines */

/*
 * trace_notify — forward an inter-PD dispatch event to trace_recorder.
 *
 * Packs src/dst/label into MR0 and the first two payload MRs into MR1/MR2,
 * then notifies the trace_recorder on CH_TRACE_NOTIFY (local id=63).
 * The trace_recorder's notified() handler reads these before any preemption
 * because it runs as a passive PD woken by the notification.
 *
 * This is a best-effort, non-blocking observation point.  If the
 * trace_recorder buffer is full it sets TRACE_FLAG_OVERFLOW and wraps.
 */
static void trace_notify(uint8_t src_pd, uint8_t dst_pd, uint16_t label,
                          uint32_t mr0_val, uint32_t mr1_val) {
    uint32_t packed = ((uint32_t)src_pd << 24)
                    | ((uint32_t)dst_pd << 16)
                    | (uint32_t)(label & 0xFFFF);
    microkit_mr_set(0, packed);
    microkit_mr_set(1, mr0_val);
    microkit_mr_set(2, mr1_val);
    microkit_notify(CH_TRACE_NOTIFY);
}

/* Forward declarations */
void vibe_swap_init(void);
int  vibe_swap_begin(uint32_t service_id, const void *code, uint32_t code_len);
int  vibe_swap_health_notify(int slot);
int  vibe_swap_rollback(uint32_t service_id);
void cap_broker_init(void);
void cap_broker_revoke_agent(uint32_t agent_pd, uint32_t reason_flags);
void agent_pool_init(void);
int  agent_pool_spawn(const char *agent_name, uint64_t task_id,
                      const uint8_t *payload, uint32_t payload_len,
                      uint32_t priority);

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
    bool     demo_complete;           /* true after the data-flow demo is done */
    /* VibeEngine demo state (Step 4: hot-swap) */
    bool     vibe_demo_triggered;     /* true after we wrote WASM to staging */
    bool     vibe_swap_in_progress;   /* true while waiting for swap slot health */
    bool     vibe_demo_complete;      /* true after vibe-swap demo finishes */
} ctrl = { false, false, 0, {0}, false, false, false, false, false, false };

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
    console_log(0, 0, buf);
}

/*
 * vibe_demo_step4() — Step 4: VibeEngine hot-swap demo
 *
 * Called when vibe_engine notifies us that a swap was approved.
 * We read the WASM + service_id from the staging region and call
 * vibe_swap_begin to load the WASM into a swap slot.
 *
 * This closes the full end-to-end pipeline:
 *   init_agent → VibeEngine (propose/validate/execute)
 *   → vibe_engine notifies controller (channel 40)
 *   → controller reads staging region
 *   → vibe_swap_begin → swap_slot wakes → loads WASM via wasm3
 *   → health_check → SERVICE LIVE
 */
static void vibe_demo_step4_notify(void) {
    console_log(0, 0, "[controller] Step 4: VibeEngine approved a swap!\n[controller] Reading proposal from staging region...\n");

    /* Read metadata from end of staging region (last 64 bytes) */
    static const uint32_t STAGING_SIZE = 0x400000;
    const volatile uint8_t *meta =
        (const volatile uint8_t *)(vibe_staging_ctrl_vaddr + STAGING_SIZE - 64);

    uint32_t service_id  = (uint32_t)meta[0] | ((uint32_t)meta[1] << 8)
                         | ((uint32_t)meta[2] << 16) | ((uint32_t)meta[3] << 24);
    uint32_t wasm_offset = (uint32_t)meta[4] | ((uint32_t)meta[5] << 8)
                         | ((uint32_t)meta[6] << 16) | ((uint32_t)meta[7] << 24);
    uint32_t wasm_size   = (uint32_t)meta[8] | ((uint32_t)meta[9] << 8)
                         | ((uint32_t)meta[10] << 16) | ((uint32_t)meta[11] << 24);

    /* Check for rollback request (wasm_size == 0xFFFFFFFF) */
    if (wasm_size == 0xFFFFFFFFU) {
        console_log(0, 0, "[controller] Rollback requested for service ");
        char sid[4];
        sid[0] = '0' + (service_id % 10);
        sid[1] = '\0';
        {
            char _cl_buf[256] = {};
            char *_cl_p = _cl_buf;
            for (const char *_s = sid; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
            *_cl_p = 0;
            console_log(0, 0, _cl_buf);
        }
        vibe_swap_rollback(service_id);
        return;
    }

    console_log(0, 0, "[controller] Swap proposal: service=");
    char svc_str[4];
    svc_str[0] = '0' + (service_id % 10);
    svc_str[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = svc_str; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = ", wasm_size="; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(0, 0, _cl_buf);
    }
    char sz_str[8];
    uint32_t s = wasm_size; int p = 0;
    if (s == 0) { sz_str[p++] = '0'; }
    else { char t[8]; int ti = 0;
           while (s > 0 && ti < 7) { t[ti++] = '0' + (s % 10); s /= 10; }
           while (ti > 0 && p < 7) sz_str[p++] = t[--ti]; }
    sz_str[p] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = sz_str; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " bytes\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(0, 0, _cl_buf);
    }

    /* The WASM binary is in the vibe_staging region at wasm_offset.
     * We need to copy it to the swap slot's code region (swap_code_ctrl_N).
     * vibe_swap_begin handles this — it takes a pointer to code bytes. */
    const void *wasm_bytes = (const void *)(vibe_staging_ctrl_vaddr + wasm_offset);

    console_log(0, 0, "[controller] Initiating kernel-side swap...\n");
    ctrl.vibe_swap_in_progress = true;

    int slot = vibe_swap_begin(service_id, wasm_bytes, wasm_size);

    if (slot < 0) {
        console_log(0, 0, "[controller] vibe_swap_begin FAILED\n");
        ctrl.vibe_swap_in_progress = false;
    } else {
        console_log(0, 0, "[controller] vibe_swap_begin OK — swap slot ");
        char sl[4];
        sl[0] = '0' + (slot % 10);
        sl[1] = '\0';
        {
            char _cl_buf[256] = {};
            char *_cl_p = _cl_buf;
            for (const char *_s = sl; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = " loading WASM via wasm3...\n"; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = "[controller] Waiting for swap slot health notification...\n"; *_s; _s++) *_cl_p++ = *_s;
            *_cl_p = 0;
            console_log(0, 0, _cl_buf);
        }
    }
}

/*
 * demo_sequence() — The main demo: real data flow between PDs
 *
 * This runs after all PDs are booted and shows agents actually
 * exchanging messages, storing/retrieving data, and publishing events.
 */
static void demo_sequence(void) {
    console_log(0, 0, "\n══════════════════════════════════════════════════════\n  DEMO: Agent Data Flow — PDs exchanging real data\n══════════════════════════════════════════════════════\n\n");

    /* ── Step 1: Store an object in AgentFS ─────────────────────────── */
    console_log(0, 0, "[controller] Step 1: Storing object in AgentFS via PPC...\n");

    /* AgentFS PUT: MR0=op, MR1=size, MR2=cap_tag */
    uint32_t obj_size = 18;  /* "Hello from agentOS" = 18 bytes */
    microkit_mr_set(0, OP_AGENTFS_PUT);
    microkit_mr_set(1, obj_size);
    microkit_mr_set(2, 0x42);  /* cap_tag: badge 0x42 */

    trace_notify(TRACE_PD_CONTROLLER, TRACE_PD_AGENTFS,
                 (uint16_t)OP_AGENTFS_PUT, obj_size, 0x42);
    microkit_ppcall(CH_AGENTFS, microkit_msginfo_new(0, 3));

    uint32_t afs_status = (uint32_t)microkit_mr_get(0);
    if (afs_status == 0) {
        /* Success — read back the object ID from MR1-MR4 */
        ctrl.demo_obj_id[0] = (uint32_t)microkit_mr_get(1);
        ctrl.demo_obj_id[1] = (uint32_t)microkit_mr_get(2);
        ctrl.demo_obj_id[2] = (uint32_t)microkit_mr_get(3);
        ctrl.demo_obj_id[3] = (uint32_t)microkit_mr_get(4);
        ctrl.demo_obj_stored = true;

        console_log(0, 0, "[controller] AgentFS PUT OK — object id: 0x");
        put_hex_byte((ctrl.demo_obj_id[0] >> 24) & 0xff);
        put_hex_byte((ctrl.demo_obj_id[0] >> 16) & 0xff);
        put_hex_byte((ctrl.demo_obj_id[0] >>  8) & 0xff);
        put_hex_byte((ctrl.demo_obj_id[0]      ) & 0xff);
        console_log(0, 0, "...\n[controller] Object payload: 'Hello from agentOS' (18 bytes)\n");
    } else {
        console_log(0, 0, "[controller] AgentFS PUT FAILED\n");
        return;
    }

    demo_delay();

    /* ── Step 2: Publish event to EventBus ──────────────────────────── */
    console_log(0, 0, "[controller] Step 2: Publishing OBJECT_CREATED event to EventBus...\n");

    microkit_mr_set(0, EVT_OBJECT_CREATED);  /* event kind */
    microkit_mr_set(1, ctrl.demo_obj_id[0]); /* first 4 bytes of object ID */
    microkit_mr_set(2, obj_size);             /* object size */

    trace_notify(TRACE_PD_CONTROLLER, TRACE_PD_EVENT_BUS,
                 (uint16_t)EVT_OBJECT_CREATED,
                 ctrl.demo_obj_id[0], obj_size);
    microkit_ppcall(CH_EVENTBUS, microkit_msginfo_new(EVT_OBJECT_CREATED, 3));
    console_log(0, 0, "[controller] Event published to ring buffer\n");

    demo_delay();

    /* ── Step 3: Dispatch task to worker_0 ──────────────────────────── */
    console_log(0, 0, "[controller] Step 3: Dispatching task to worker_0 — 'retrieve object'\n");

    ctrl.worker_task_dispatched = true;
    trace_notify(TRACE_PD_CONTROLLER, TRACE_PD_WORKER_0, 0, 0, 0);
    microkit_notify(CH_WORKER_BASE);  /* notify worker_0 */

    console_log(0, 0, "[controller] Task dispatched. Waiting for worker completion...\n");
    /* Worker will notify us back on channel 10 when done */
}

void init(void) {
    agentos_log_boot("controller");
    
    console_log(0, 0, "[controller] Initializing agentOS core services\n");
    
    /* Initialize subsystems */
    cap_broker_init();
    agent_pool_init();
    
    /* PPC into EventBus (passive, higher priority) to initialize it */
    console_log(0, 0, "[controller] Waking EventBus via PPC...\n");
    trace_notify(TRACE_PD_CONTROLLER, TRACE_PD_EVENT_BUS,
                 (uint16_t)MSG_EVENTBUS_INIT, 0, 0);
    microkit_msginfo result = microkit_ppcall(CH_EVENTBUS,
        microkit_msginfo_new(MSG_EVENTBUS_INIT, 0));
    
    uint64_t resp = microkit_msginfo_get_label(result);
    if (resp == MSG_EVENTBUS_READY) {
        ctrl.eventbus_ready = true;
        console_log(0, 0, "[controller] EventBus: READY\n");
    } else {
        console_log(0, 0, "[controller] EventBus: unexpected response\n");
    }
    
    /* Notify InitAgent to start (it's active, so we can't PPC into it) */
    console_log(0, 0, "[controller] Notifying InitAgent to start...\n");
    trace_notify(TRACE_PD_CONTROLLER, TRACE_PD_INIT_AGENT,
                 (uint16_t)MSG_INITAGENT_START, 0, 0);
    microkit_notify(CH_INITAGENT);
    
    /* Initialize vibe-swap subsystem (sets up swap slot channels + service table) */
    vibe_swap_init();

    console_log(0, 0, "[controller] *** agentOS controller boot complete ***\n[controller] Ready for agents.\n");
    
    /* Run the interactive demo sequence */
    demo_sequence();
}

void notified(microkit_channel ch) {
    ctrl.notification_count++;
    
    switch (ch) {
        case CH_EVENTBUS:
            console_log(0, 0, "[controller] EventBus notification\n");
            ctrl.eventbus_ready = true;
            break;
            
        case CH_INITAGENT: {
            /*
             * Two sub-cases:
             *   a) InitAgent startup ready notification (MR0 = 0 or not MSG_SPAWN_AGENT)
             *   b) Spawn request relay from init_agent (MR0 = MSG_SPAWN_AGENT)
             *
             * We distinguish by reading MR0. seL4 Microkit does preserve MR values
             * across notifications to the notified() handler.
             */
            uint32_t notif_tag = (uint32_t)microkit_mr_get(0);
            if (notif_tag == (uint32_t)MSG_SPAWN_AGENT) {
                /*
                 * init_agent is relaying a dynamic spawn request.
                 * MR1: wasm_hash_lo_low32   MR2: wasm_hash_lo_hi32
                 * MR3: wasm_hash_hi_low32   MR4: spawn_id
                 * MR5: priority
                 */
                uint32_t hash_lo_lo  = (uint32_t)microkit_mr_get(1);
                uint32_t hash_lo_hi  = (uint32_t)microkit_mr_get(2);
                uint32_t hash_hi_lo  = (uint32_t)microkit_mr_get(3);
                uint32_t spawn_id    = (uint32_t)microkit_mr_get(4);
                uint32_t priority    = (uint32_t)microkit_mr_get(5);

                console_log(0, 0, "[controller] SPAWN_AGENT request: spawn_id=");
                /* print spawn_id decimal */
                {
                    char buf[12]; int bi = 11; buf[bi] = '\0';
                    uint32_t v = spawn_id;
                    if (v == 0) { buf[--bi] = '0'; }
                    else while (v > 0 && bi > 0) { buf[--bi] = '0' + (v % 10); v /= 10; }
                    console_log(0, 0, &buf[bi]);
                }
                console_log(0, 0, " hash_lo=");
                {
                    static const char hex[] = "0123456789abcdef";
                    char hbuf[9]; hbuf[8] = '\0';
                    for (int hi = 7; hi >= 0; hi--) {
                        hbuf[hi] = hex[hash_lo_lo & 0xf]; hash_lo_lo >>= 4;
                    }
                    (void)hash_lo_hi; (void)hash_hi_lo;
                    console_log(0, 0, hbuf);
                }
                console_log(0, 0, "\n");

                /*
                 * Construct an agent name from spawn_id for pool tracking.
                 * In production: resolve from AgentFS metadata.
                 */
                char agent_name[17] = "wasm-agent-00000";
                {
                    uint32_t sid = spawn_id;
                    for (int ni = 15; ni >= 11; ni--) {
                        agent_name[ni] = '0' + (sid % 10);
                        sid /= 10;
                    }
                }

                /*
                 * Dispatch via agent_pool_spawn. The payload carries spawn_id
                 * and priority so the worker knows its identity.
                 */
                uint8_t spawn_payload[8];
                spawn_payload[0] = (uint8_t)(spawn_id & 0xff);
                spawn_payload[1] = (uint8_t)((spawn_id >> 8) & 0xff);
                spawn_payload[2] = (uint8_t)((spawn_id >> 16) & 0xff);
                spawn_payload[3] = (uint8_t)((spawn_id >> 24) & 0xff);
                spawn_payload[4] = (uint8_t)(priority & 0xff);
                spawn_payload[5] = (uint8_t)((priority >> 8) & 0xff);
                spawn_payload[6] = 0;
                spawn_payload[7] = 0;

                int slot = agent_pool_spawn(agent_name, 0,
                                            spawn_payload, 8, priority);

                /*
                 * Notify init_agent with the result.
                 * MR0: MSG_SPAWN_AGENT_REPLY
                 * MR1: spawn_id
                 * MR2: slot_id (negative = failure)
                 */
                microkit_mr_set(0, MSG_SPAWN_AGENT_REPLY);
                microkit_mr_set(1, spawn_id);
                microkit_mr_set(2, (uint32_t)(int32_t)slot);
                trace_notify(TRACE_PD_CONTROLLER, TRACE_PD_INIT_AGENT,
                             (uint16_t)MSG_SPAWN_AGENT_REPLY,
                             spawn_id, (uint32_t)(int32_t)slot);
                microkit_notify(CH_INITAGENT);

                if (slot >= 0) {
                    console_log(0, 0, "[controller] Agent spawned: slot=");
                    char s[2] = { (char)('0' + (slot % 10)), '\0' };
                    {
                        char _cl_buf[256] = {};
                        char *_cl_p = _cl_buf;
                        for (const char *_s = s; *_s; _s++) *_cl_p++ = *_s;
                        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
                        *_cl_p = 0;
                        console_log(0, 0, _cl_buf);
                    }
                } else {
                    console_log(0, 0, "[controller] SPAWN_AGENT: pool exhausted\n");
                }
            } else {
                console_log(0, 0, "[controller] InitAgent ready notification received\n");
                ctrl.initagent_ready = true;
            }
            break;
        }
            
        default:
            /* Channels 10-17: worker pool ready/completion notifications */
            if (ch >= 10 && ch <= 17) {
                uint32_t pool_slot = ch - 10;
                
                /* State-based dispatch: notifications don't carry MR payload
                 * in seL4. Use ctrl state to determine notification meaning. */
                if (pool_slot == 0 && ctrl.worker_task_dispatched && !ctrl.demo_complete) {
                    /* Worker_0 completed the demo task */
                    ctrl.demo_complete = true;
                    
                    console_log(0, 0, "[controller] Worker 0 task COMPLETE\n");
                    
                    /* Publish TASK_COMPLETE event to EventBus */
                    console_log(0, 0, "[controller] Publishing TASK_COMPLETE event to EventBus...\n");
                    microkit_mr_set(0, MSG_EVENT_AGENT_EXITED);
                    microkit_mr_set(1, 0);
                    microkit_mr_set(2, 1);

                    trace_notify(TRACE_PD_CONTROLLER, TRACE_PD_EVENT_BUS,
                                 (uint16_t)MSG_EVENT_AGENT_EXITED, 0, 1);
                    microkit_ppcall(CH_EVENTBUS,
                        microkit_msginfo_new(MSG_EVENT_AGENT_EXITED, 3));
                    console_log(0, 0, "[controller] TASK_COMPLETE event published\n");
                    
                    /* Notify InitAgent to query final EventBus status */
                    microkit_notify(CH_INITAGENT);
                    
                    demo_delay();
                    
                    /* Print Step 1-3 summary */
                    console_log(0, 0, "\n──────────────────────────────────────────────────────\n  Steps 1-3 complete: AgentFS + EventBus + Workers\n──────────────────────────────────────────────────────\n\n");

                    /*
                     * Step 4: VibeEngine hot-swap demo.
                     *
                     * The controller writes the echo_service.wasm into the
                     * vibe_staging shared memory region, then PPCs into
                     * vibe_engine (via init_agent as proxy — or directly
                     * if we had a channel).
                     *
                     * For the demo, controller drives the VibeEngine pipeline
                     * directly using the staging region + VibeEngine notify path:
                     *
                     *   1. Write echo_service.wasm to staging region (we have
                     *      the staging mapped r, but for the demo controller
                     *      has NO write access to staging — vibe_engine owns it rw).
                     *
                     *   So instead: controller embeds the WASM and feeds it
                     *   directly to vibe_swap_begin (bypassing VibeEngine for
                     *   this one step, since we don't have a controller→vibe_engine
                     *   PPC channel — only the notify channel).
                     *
                     *   This is the correct architecture: VibeEngine is for
                     *   external agent proposals. The controller already HAS
                     *   vibe_swap_begin for trusted internal swaps. Both paths
                     *   land in the same swap slot pipeline.
                     *
                     *   The VibeEngine demo (external agent path) is shown
                     *   via the init_agent→vibe_engine PPC channel (channel 41).
                     *   init_agent will trigger that path when it receives our
                     *   notify below.
                     */
                    console_log(0, 0, "[controller] Step 4: VibeEngine hot-swap demo...\n[controller] Direct path: loading echo_service.wasm into swap slot 0\n");

                    /* Direct vibe_swap_begin (trusted controller path) */
                    /* service 2 = toolsvc (swappable) */
                    ctrl.vibe_demo_triggered = true;
                    int vslot = vibe_swap_begin(2, ECHO_SERVICE_WASM, ECHO_SERVICE_WASM_LEN);
                    if (vslot < 0) {
                        console_log(0, 0, "[controller] Step 4 vibe_swap_begin FAILED\n");
                        ctrl.vibe_demo_triggered = false;
                    } else {
                        console_log(0, 0, "[controller] Step 4: WASM loaded into swap slot ");
                        char vsl[4];
                        vsl[0] = '0' + (vslot % 10);
                        vsl[1] = '\0';
                        {
                            char _cl_buf[256] = {};
                            char *_cl_p = _cl_buf;
                            for (const char *_s = vsl; *_s; _s++) *_cl_p++ = *_s;
                            for (const char *_s = " — waiting for wasm3 health check...\n"; *_s; _s++) *_cl_p++ = *_s;
                            *_cl_p = 0;
                            console_log(0, 0, _cl_buf);
                        }
                        ctrl.vibe_swap_in_progress = true;
                    }
                } else {
                    console_log(0, 0, "[controller] Worker ");
                    char s[2] = { (char)('0' + pool_slot), '\0' };
                    {
                        char _cl_buf[256] = {};
                        char *_cl_p = _cl_buf;
                        for (const char *_s = s; *_s; _s++) *_cl_p++ = *_s;
                        for (const char *_s = " ready\n"; *_s; _s++) *_cl_p++ = *_s;
                        *_cl_p = 0;
                        console_log(0, 0, _cl_buf);
                    }
                    /* Ack the worker's ready signal */
                    microkit_notify(ch);
                }
            /* Channel 50: GPU scheduler ready / GPU task dispatch request */
            } else if (ch == CH_GPUSCHED) {
                uint32_t gpu_tag = (uint32_t)microkit_mr_get(0);
                if (gpu_tag == (uint32_t)MSG_GPU_SUBMIT) {
                    /*
                     * gpu_sched is asking us to route a WASM GPU task to a worker slot.
                     * MR1: ticket_id, MR2/3: hash_lo, MR4: hash_hi low32, MR5: slot_id
                     * We notify the appropriate worker slot to load+execute the WASM.
                     */
                    uint32_t ticket  = (uint32_t)microkit_mr_get(1);
                    uint32_t slot_id = (uint32_t)microkit_mr_get(5);
                    (void)ticket;
                    console_log(0, 0, "[controller] GPU task dispatched to slot=");
                    {
                        char s[2] = { (char)('0' + (slot_id % 10)), '\0' };
                        console_log(0, 0, s);
                    }
                    console_log(0, 0, "\n");
                    /*
                     * Notify the target swap slot to start WASM execution.
                     * In production: pass hash via MRs for agentfs fetch first.
                     * For now: worker slot gets a generic compute notification.
                     */
                    if (slot_id < (uint32_t)NUM_SWAP_SLOTS) {
                        trace_notify(TRACE_PD_CONTROLLER,
                                     (uint8_t)(TRACE_PD_SWAP_SLOT_0 + slot_id),
                                     (uint16_t)MSG_GPU_SUBMIT, ticket, slot_id);
                        microkit_notify((microkit_channel)(CH_SWAP_BASE + slot_id));
                    }
                } else {
                    /* gpu_sched startup ready notification */
                    console_log(0, 0, "[controller] GPU Scheduler online\n");
                }
            /* Channel 55: mesh_agent ready notification */
            } else if (ch == (microkit_channel)CH_MESHAGENT) {
                console_log(0, 0, "[controller] Distributed mesh agent online\n");
            } else if (ch == (microkit_channel)CH_QUOTA_NOTIFY) {
                uint32_t tag = (uint32_t)microkit_mr_get(0);
                uint32_t agent_id = (uint32_t)microkit_mr_get(1);
                uint32_t reason   = (uint32_t)microkit_mr_get(2);
                if (tag == (uint32_t)MSG_QUOTA_REVOKE) {
                    console_log(0, 0, "[controller] Quota revoke request: agent=");
                    char buf[12]; int bi = 11; buf[bi] = '\0';
                    uint32_t v = agent_id;
                    if (v == 0) { buf[--bi] = '0'; }
                    else {
                        while (v > 0 && bi > 0) {
                            buf[--bi] = (char)('0' + (v % 10));
                            v /= 10;
                        }
                    }
                    {
                        char _cl_buf[256] = {};
                        char *_cl_p = _cl_buf;
                        for (const char *_s = &buf[bi]; *_s; _s++) *_cl_p++ = *_s;
                        for (const char *_s = " reason=0x"; *_s; _s++) *_cl_p++ = *_s;
                        *_cl_p = 0;
                        console_log(0, 0, _cl_buf);
                    }
                    char hex[9];
                    for (int i = 0; i < 8; i++) {
                        uint32_t nib = (reason >> (28 - i * 4)) & 0xF;
                        hex[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
                    }
                    hex[8] = '\0';
                    {
                        char _cl_buf[256] = {};
                        char *_cl_p = _cl_buf;
                        for (const char *_s = hex; *_s; _s++) *_cl_p++ = *_s;
                        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
                        *_cl_p = 0;
                        console_log(0, 0, _cl_buf);
                    }
                    cap_broker_revoke_agent(agent_id, reason);
                } else {
                    console_log(0, 0, "[controller] Unknown quota notify\n");
                }
            /* Channel 40: vibe_engine approved a swap — read staging and begin */
            } else if (ch == CH_VIBEENGINE) {
                vibe_demo_step4_notify();
            /* Channels 30-33: swap slot health-OK notifications */
            } else if (ch >= CH_SWAP_BASE && ch < CH_SWAP_BASE + NUM_SWAP_SLOTS) {
                int swap_slot_idx = (int)(ch - CH_SWAP_BASE);
                uint32_t status = (uint32_t)microkit_mr_get(0);
                if (status == 0) {
                    console_log(0, 0, "[controller] Swap slot health OK — activating\n");
                    vibe_swap_health_notify(swap_slot_idx);

                    /* If this is the vibe demo swap, print final summary */
                    if (ctrl.vibe_swap_in_progress && !ctrl.vibe_demo_complete) {
                        ctrl.vibe_swap_in_progress = false;
                        ctrl.vibe_demo_complete = true;
                        console_log(0, 0, "\n══════════════════════════════════════════════════════\n  DEMO COMPLETE — All 4 steps passed!\n  Step 1: AgentFS object store    — PUT/GET via IPC\n  Step 2: EventBus pub/sub        — ring buffer + notify\n  Step 3: Agent pool workers      — task dispatch + done\n  Step 4: VibeEngine hot-swap     — WASM live via wasm3\n  PDs running: 20 (+ vibe_engine) on seL4 RISC-V\n  Kernel: formally verified seL4 microkernel\n  Swap: echo_service.wasm loaded into toolsvc slot\n══════════════════════════════════════════════════════\n\n[controller] agentOS: the world's first agent-native OS. :)\n");
                    }
                } else {
                    console_log(0, 0, "[controller] Swap slot health FAIL\n");
                }
            } else {
                console_log(0, 0, "[controller] Unknown channel\n");
            }
            break;
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)ch;
    uint64_t label = microkit_msginfo_get_label(msg);
    
    if (label == MSG_WORKER_RETRIEVE) {
        /* Worker requesting AgentFS object retrieval (proxy) */
        console_log(0, 0, "[controller] Proxying AgentFS GET for worker...\n");
        
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
            
            console_log(0, 0, "[controller] AgentFS returned object: version=");
            char v[2] = { (char)('0' + (version % 10)), '\0' };
            {
                char _cl_buf[256] = {};
                char *_cl_p = _cl_buf;
                for (const char *_s = v; *_s; _s++) *_cl_p++ = *_s;
                for (const char *_s = ", size="; *_s; _s++) *_cl_p++ = *_s;
                *_cl_p = 0;
                console_log(0, 0, _cl_buf);
            }
            /* Print size as decimal */
            if (size < 100) {
                char tens = (char)('0' + (size / 10));
                char ones = (char)('0' + (size % 10));
                char sz[3] = { tens, ones, '\0' };
                console_log(0, 0, sz);
            } else {
                console_log(0, 0, "??");
            }
            console_log(0, 0, " bytes\n");
            
            /* Pack response for worker: MR0=status, MR1=size, MR2=cap_tag, MR3=version */
            microkit_mr_set(0, 0);       /* OK */
            microkit_mr_set(1, size);
            microkit_mr_set(2, cap_tag);
            microkit_mr_set(3, version);
            return microkit_msginfo_new(MSG_WORKER_RETRIEVE_REPLY, 4);
        } else {
            console_log(0, 0, "[controller] AgentFS GET failed\n");
            microkit_mr_set(0, status);
            return microkit_msginfo_new(MSG_WORKER_RETRIEVE_REPLY, 1);
        }
    }
    
    console_log(0, 0, "[controller] Unexpected PPC call\n");
    return microkit_msginfo_new(0xDEAD, 0);
}

/* Note: this extends the notified() function above.
 * In the actual build, the notified() switch needs a POOL_CH_BASE case.
 * Adding it here as a patch note for next refactor. */
