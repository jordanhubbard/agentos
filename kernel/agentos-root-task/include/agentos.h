/*
 * agentOS - kernel layer common header
 * 
 * All protection domains include this header.
 * It provides:
 * - agentOS version constants
 * - Channel ID definitions (must match agentos.system)
 * - Common IPC message formats
 * - Debug logging macros
 */

#pragma once

#include <microkit.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* agentOS version */
#define AGENTOS_VERSION_MAJOR 0
#define AGENTOS_VERSION_MINOR 1
#define AGENTOS_VERSION_PATCH 0
#define AGENTOS_VERSION_STR   "agentOS v0.1.0-alpha"

/*
 * Channel IDs (must match agentos.system)
 * Each PD sees channels from its own perspective.
 */

/* Monitor PD channels */
#define MONITOR_CH_EVENTBUS   1
#define MONITOR_CH_INITAGENT  2

/* EventBus PD channels */
#define EVENTBUS_CH_MONITOR   1
#define EVENTBUS_CH_INITAGENT 2

/* InitAgent PD channels */
#define INITAGENT_CH_MONITOR  1
#define INITAGENT_CH_EVENTBUS 2

/* Swap Slot PD channels (from swap slot perspective) */
#define SWAPSLOT_CH_CONTROLLER 0

/* Swap Slot channel IDs (from controller perspective) */
#define SWAP_SLOT_BASE_CH     8   /* Channels 8-11 are swap slots */
#define MAX_SWAP_SLOTS        4

/* Worker Pool channel IDs (from controller perspective) */
#define WORKER_POOL_BASE_CH   20  /* Channels 20-27 are worker pool */
#define WORKER_POOL_SIZE      8

/*
 * IPC Message Tags
 * Packed into the microkit_msginfo label field (bits 0-63)
 */
typedef enum {
    /* Monitor -> EventBus */
    MSG_EVENTBUS_INIT          = 0x0001,
    MSG_EVENTBUS_SUBSCRIBE     = 0x0002,
    MSG_EVENTBUS_UNSUBSCRIBE   = 0x0003,
    MSG_EVENTBUS_STATUS        = 0x0004,

    /* EventBus -> Monitor */
    MSG_EVENTBUS_READY         = 0x0101,
    MSG_EVENTBUS_ERROR         = 0x0102,

    /* Monitor -> InitAgent */
    MSG_INITAGENT_START        = 0x0201,
    MSG_INITAGENT_SHUTDOWN     = 0x0202,

    /* InitAgent -> Monitor */  
    MSG_INITAGENT_READY        = 0x0301,
    MSG_INITAGENT_STATUS       = 0x0302,

    /* EventBus -> InitAgent (events) */
    MSG_EVENT_AGENT_SPAWNED    = 0x0401,
    MSG_EVENT_AGENT_EXITED     = 0x0402,
    MSG_EVENT_SYSTEM_READY     = 0x0403,

    /* EventBus batch publish */
    MSG_EVENTBUS_PUBLISH_BATCH = 0x0005,  /* publish up to 16 events in one PPC */

    /* AgentFS event types (published to EventBus) */
    MSG_EVENT_PUBLISH          = 0x0410,  /* generic EventBus publish op */
    EVT_OBJECT_CREATED         = 0x0411,  /* new object stored */
    EVT_OBJECT_DELETED         = 0x0412,  /* object tombstoned */
    EVT_OBJECT_EVICTED         = 0x0413,  /* object moved to cold tier */

    /* Worker <-> Controller task ops */
    MSG_WORKER_RETRIEVE        = 0x0701,  /* Worker asks controller to GET from AgentFS */
    MSG_WORKER_RETRIEVE_REPLY  = 0x0702,  /* Controller returns AgentFS data to worker */
    MSG_DEMO_TASK_RETRIEVE     = 0x0710,  /* Task type: retrieve object from AgentFS */

    /* Vibe Swap (VibeEngine -> Controller -> Swap Slots) */
    MSG_VIBE_SWAP_BEGIN        = 0x0501,  /* VibeEngine -> Controller: start swap */
    MSG_VIBE_SWAP_ACTIVATE     = 0x0502,  /* Controller -> slot: go live */
    MSG_VIBE_SWAP_ROLLBACK     = 0x0503,  /* Controller: revert to previous */
    MSG_VIBE_SWAP_HEALTH       = 0x0504,  /* Controller -> slot: health check */
    MSG_VIBE_SWAP_STATUS       = 0x0505,  /* Query swap status */
    MSG_VIBE_SLOT_READY        = 0x0601,  /* Swap slot -> Controller: loaded OK */
    MSG_VIBE_SLOT_FAILED       = 0x0602,  /* Swap slot -> Controller: load failed */
    MSG_VIBE_SLOT_HEALTHY      = 0x0603,  /* Swap slot -> Controller: health OK */

    /* VibeEngine IPC operations (agents -> vibe_engine PD) */
    MSG_VIBE_NOTIFY_SWAP       = 0x0700,  /* vibe_engine -> controller: swap approved */
    MSG_VIBE_NOTIFY_ROLLBACK   = 0x0701,  /* vibe_engine -> controller: rollback */

} agentos_msg_tag_t;

/*
 * VibeEngine op codes (MR0 field in PPC requests to vibe_engine PD)
 */
#define OP_VIBE_PROPOSE   0x40
#define OP_VIBE_VALIDATE  0x41
#define OP_VIBE_EXECUTE   0x42
#define OP_VIBE_STATUS    0x43
#define OP_VIBE_ROLLBACK  0x44
#define OP_VIBE_HEALTH    0x45

/* VibeEngine channel IDs (from controller perspective) */
#define CH_VIBEENGINE         40  /* controller <-> vibe_engine (notify) */

/* Console Multiplexer channel IDs (from controller perspective) */
#ifdef BOARD_qemu_virt_aarch64
#define CH_CONSOLEMUX         55  /* aarch64: 50 is linux_vmm */
#else
#define CH_CONSOLEMUX         60  /* riscv64: after mem_profiler (50-58) */
#endif

/* Console Multiplexer op codes (MR0 in PPC to console_mux) */
#define OP_CONSOLE_ATTACH     0x80  /* MR1=pd_id: attach to PD output */
#define OP_CONSOLE_DETACH     0x81  /* detach, revert to broadcast */
#define OP_CONSOLE_LIST       0x82  /* list sessions, MR1=bitmask */
#define OP_CONSOLE_MODE       0x83  /* MR1=mode: 0=single,1=broadcast,2=split */
#define OP_CONSOLE_INJECT     0x84  /* MR1..4=chars: inject input */
#define OP_CONSOLE_SCROLL     0x85  /* MR1=lines: scroll back */
#define OP_CONSOLE_STATUS     0x86  /* returns: MR1=active_pd, MR2=mode, MR3=count */
#define OP_CONSOLE_WRITE      0x87  /* PD->mux: register ring + flush */

/* VibeEngine staging region metadata layout (last 64 bytes of 4MB staging MR) */
#define VIBE_META_SIZE        64
/* meta[0..3]   = service_id (LE uint32) */
/* meta[4..7]   = wasm_offset (LE uint32, offset into staging region) */
/* meta[8..11]  = wasm_size (LE uint32; 0xFFFFFFFF = rollback request) */
/* meta[12..15] = proposal_id (LE uint32) */
#define VIBE_META_MAGIC_ROLLBACK  0xFFFFFFFFU

/*
 * Priority classes (mapped to seL4 MCS scheduling context budgets)
 */
typedef enum {
    PRIO_HARD_RT    = 250,
    PRIO_MONITOR    = 254,
    PRIO_EVENTBUS   = 200,
    PRIO_SOFT_RT    = 150,
    PRIO_INTERACTIVE= 128,
    PRIO_COMPUTE    = 80,
    PRIO_BACKGROUND = 10,
    PRIO_IDLE       = 0,
} agentos_priority_t;

/*
 * Debug logging
 * In release builds, these compile to nothing.
 */
#ifdef AGENTOS_DEBUG
#  define AGENTOS_LOG(fmt, ...) \
     do { \
         microkit_dbg_puts("[agentos] "); \
         microkit_dbg_puts(fmt);          \
         microkit_dbg_puts("\n");         \
     } while(0)
#else
#  define AGENTOS_LOG(fmt, ...) do {} while(0)
#endif

/* Always-on critical logs */
#define AGENTOS_CRIT(fmt) \
    do { \
        microkit_dbg_puts("[CRIT] "); \
        microkit_dbg_puts(fmt);       \
        microkit_dbg_puts("\n");      \
    } while(0)

/*
 * Agent ID type
 * Packed 128-bit identifier: namespace(32) | name(64) | epoch(16) | random(16)
 * Simplified representation for C layer; full ID in Rust SDK.
 */
typedef struct {
    uint64_t high;
    uint64_t low;
} agentos_agent_id_t;

/*
 * Fault descriptor - what went wrong
 */
typedef struct {
    uint32_t kind;        /* fault kind enum */
    uint64_t addr;        /* faulting address (if applicable) */
    uint64_t ip;          /* instruction pointer at fault */
    uint32_t pid;         /* protection domain id */
} agentos_fault_t;

/*
 * EventBus ring buffer entry
 * The ring buffer is in shared memory between EventBus and subscribers.
 */
typedef struct __attribute__((packed)) {
    uint64_t seq;         /* sequence number */
    uint64_t timestamp_ns;
    uint32_t kind;        /* event kind */
    uint32_t source_pd;   /* source protection domain */
    uint32_t payload_len; /* payload length in bytes */
    uint8_t  payload[64]; /* inline payload (up to 64 bytes) */
} agentos_event_t;

/* EventBus ring buffer header (at start of shared memory region) */
typedef struct __attribute__((packed)) {
    uint32_t magic;       /* 0xA6E7_0B05 = "AGENTOS" */
    uint32_t version;
    uint64_t capacity;    /* number of event slots */
    uint64_t head;        /* write index */
    uint64_t tail;        /* read index */
    uint8_t  _pad[40];   /* pad to 64 bytes */
} agentos_ring_header_t;

#define AGENTOS_RING_MAGIC 0xA6E70B05

/*
 * Batch publish support (OP_PUBLISH_BATCH / MSG_EVENTBUS_PUBLISH_BATCH)
 *
 * Callers fill up to EVENTBUS_BATCH_MAX compact event descriptors into the
 * batch staging area — a fixed window at the end of the shared 256 KB ring
 * region — then invoke a single PPC with the event count in MR[1].  The
 * EventBus PD copies all entries into the main ring and calls
 * eventbus_notify_all() exactly once, amortising the notify cost.
 *
 * Layout of the 256 KB eventbus_ring region (0x40000 bytes):
 *   [0 .. EVENTBUS_BATCH_STAGING_OFFSET)  — ring header + event slots
 *   [EVENTBUS_BATCH_STAGING_OFFSET .. 0x40000) — batch staging (768 bytes)
 */
#define EVENTBUS_BATCH_MAX          16U
#define EVENTBUS_BATCH_EVENT_SIZE   48U   /* sizeof(agentos_batch_event_t) */
#define EVENTBUS_BATCH_STAGING_OFFSET \
    (0x40000U - EVENTBUS_BATCH_MAX * EVENTBUS_BATCH_EVENT_SIZE)

/*
 * Compact event descriptor used only in the batch staging area.
 * Payload is truncated to 36 bytes; richer payloads must use single publishes.
 */
typedef struct __attribute__((packed)) {
    uint32_t kind;
    uint32_t source_pd;
    uint32_t payload_len;   /* 0..36; clamped on write */
    uint8_t  payload[36];
} agentos_batch_event_t;

/*
 * MSGBUS_BATCH_PUSH(buf, idx, kind, source, payload, plen)
 *
 * Append one event to a batch staging buffer.
 *   buf    — volatile agentos_batch_event_t * pointing at the staging area
 *            (ring_vaddr + EVENTBUS_BATCH_STAGING_OFFSET)
 *   idx    — uint32_t counter; must be 0 on first call, incremented by macro
 *   kind   — event kind (uint32_t)
 *   source — source PD id (uint32_t)
 *   payload— const uint8_t * or NULL
 *   plen   — payload byte length (clamped to 36)
 *
 * When idx reaches EVENTBUS_BATCH_MAX the macro is a no-op (silent drop).
 * Flush with a PPC: MR[1]=idx, label=MSG_EVENTBUS_PUBLISH_BATCH.
 */
#define MSGBUS_BATCH_PUSH(buf, idx, _kind, _src, _payload, _plen)           \
    do {                                                                      \
        if ((idx) < EVENTBUS_BATCH_MAX) {                                    \
            volatile agentos_batch_event_t *_s = &(buf)[(idx)];             \
            _s->kind        = (uint32_t)(_kind);                             \
            _s->source_pd   = (uint32_t)(_src);                              \
            uint32_t _l = (uint32_t)(_plen) < 36u ? (uint32_t)(_plen) : 36u;\
            _s->payload_len = _l;                                             \
            const uint8_t *_pp = (const uint8_t *)(_payload);               \
            for (uint32_t _bi = 0; _bi < _l; _bi++)                         \
                _s->payload[_bi] = _pp ? _pp[_bi] : 0u;                     \
            (idx)++;                                                          \
        }                                                                     \
    } while (0)

/* Log function declarations */
void agentos_log_boot(const char *pd_name);
void agentos_log_info(const char *pd, const char *msg);
void agentos_log_channel(const char *pd, uint32_t ch);
void agentos_log_fault(const char *pd, agentos_fault_t *f);

/*
 * Capability descriptor (simplified for C layer)
 * Full capability management is in the Rust SDK.
 */
typedef struct {
    uint32_t cptr;
    uint32_t rights;
    uint32_t kind;
    uint32_t badge;
} agentos_cap_desc_t;

/* ── PerfCounters integration macros ────────────────────────────────────── */
/*
 * PERF_PPC(perf_ch, ppc_ch, msg, caller_id, callee_id)
 *
 * Instrument a PPC call with latency measurement via the perf_counters PD.
 * perf_ch — channel ID to perf_counters from this PD (from agentos.system)
 * ppc_ch  — channel ID of the actual PPC target
 * msg     — microkit_msginfo for the real PPC call
 * caller_id / callee_id — PD ID constants (see below)
 *
 * The macro saves MR contents, PPCs to perf_counters for BEGIN token,
 * restores MRs, makes the real PPC call, then PPCs perf_counters for END.
 *
 * Zero overhead when PERF_COUNTERS_DISABLE is defined (e.g. production builds).
 */
#define PD_ID_CONTROLLER    0
#define PD_ID_EVENT_BUS     1
#define PD_ID_INIT_AGENT    2
#define PD_ID_WORKER_BASE   3   /* worker_N = PD_ID_WORKER_BASE + N */
#define PD_ID_AGENTFS       11
#define PD_ID_VIBE_ENGINE   12
#define PD_ID_CONSOLE_MUX   13
#define PD_ID_MEM_PROFILER  14
#define PD_ID_PERF_COUNTERS 15
#define PD_ID_SWAP_SLOT_0   16  /* swap_slot_N = PD_ID_SWAP_SLOT_0 + N */

#define OP_PERF_BEGIN  0xC0
#define OP_PERF_END    0xC1

#ifndef PERF_COUNTERS_DISABLE

#define PERF_PPC(perf_ch, ppc_ch, msg, caller_id, callee_id) \
    (__extension__({ \
        /* Save MR0..MR3 set by caller */ \
        uint32_t _mr0 = (uint32_t)microkit_mr_get(0); \
        uint32_t _mr1 = (uint32_t)microkit_mr_get(1); \
        uint32_t _mr2 = (uint32_t)microkit_mr_get(2); \
        uint32_t _mr3 = (uint32_t)microkit_mr_get(3); \
        /* BEGIN: get token from perf_counters */ \
        microkit_mr_set(0, 0); \
        microkit_mr_set(1, (caller_id)); \
        microkit_mr_set(2, (callee_id)); \
        microkit_ppcall((perf_ch), microkit_msginfo_new(OP_PERF_BEGIN, 3)); \
        uint32_t _token = (uint32_t)microkit_mr_get(0); \
        /* Restore MRs and make the real PPC */ \
        microkit_mr_set(0, _mr0); \
        microkit_mr_set(1, _mr1); \
        microkit_mr_set(2, _mr2); \
        microkit_mr_set(3, _mr3); \
        microkit_msginfo _reply = microkit_ppcall((ppc_ch), (msg)); \
        /* END: report latency (don't clobber reply MRs — use saved token) */ \
        microkit_mr_set(0, 0); \
        microkit_mr_set(1, _token); \
        microkit_mr_set(2, (callee_id)); \
        microkit_ppcall((perf_ch), microkit_msginfo_new(OP_PERF_END, 3)); \
        _reply; \
    }))

#else /* PERF_COUNTERS_DISABLE */
#define PERF_PPC(perf_ch, ppc_ch, msg, caller_id, callee_id) \
    microkit_ppcall((ppc_ch), (msg))
#endif /* PERF_COUNTERS_DISABLE */

/* ── TimePartition integration ───────────────────────────────────────────── */

/* Agent class IDs for OP_TP_CONFIGURE */
#define TP_CLASS_GPU_WORKER   0
#define TP_CLASS_MESH_AGENT   1
#define TP_CLASS_WATCHDOG     2
#define TP_CLASS_INIT_AGENT   3
#define TP_CLASS_BACKGROUND   4

/* Opcodes */
#define OP_TP_CONFIGURE   0xD0
#define OP_TP_SET_POLICY  0xD1
#define OP_TP_GET_POLICY  0xD2
#define OP_TP_SUSPEND     0xD3
#define OP_TP_QUERY       0xD4
#define OP_TP_TICK        0xD5
#define OP_TP_RESET       0xD6

/*
 * TP_REGISTER(tp_ch, pd_id, class_id)
 * Register this PD with the time_partition scheduler at boot.
 * Call from init().  ch is the channel id to time_partition.
 *
 * Example (worker_0 registering as gpu_worker):
 *   TP_REGISTER(CH_TIME_PARTITION, PD_ID_WORKER_BASE + 0, TP_CLASS_GPU_WORKER);
 */
#define TP_REGISTER(tp_ch, pd_id, class_id) do { \
    microkit_mr_set(1, (pd_id)); \
    microkit_mr_set(2, (class_id)); \
    microkit_ppcall((tp_ch), microkit_msginfo_new(OP_TP_CONFIGURE, 2)); \
} while(0)

/*
 * TP_TICK(tp_ch, quantum_us)
 * Send a scheduler tick to time_partition.  Called by controller each
 * scheduling quantum (typically from a hardware timer notification).
 */
#define TP_TICK(tp_ch, quantum_us) do { \
    microkit_mr_set(1, (quantum_us)); \
    microkit_ppcall((tp_ch), microkit_msginfo_new(OP_TP_TICK, 1)); \
} while(0)

/* ── Capability Attestation ──────────────────────────────────────────────── */
#define OP_CAP_ATTEST     0xE0   /* monitor: serialize + sign cap table */
#define OP_CAP_ATTEST_GET 0xE1   /* fetch last attestation from AgentFS */

/* cap_broker_attest() — declared for inclusion in monitor.c */
uint32_t cap_broker_attest(char *buf, uint32_t buf_len, uint64_t timestamp);

/* ── Boot Integrity Measurement ─────────────────────────────────────────── */
#define OP_BOOT_MEASURE  0xB0
#define OP_BOOT_SEAL     0xB1
#define OP_BOOT_QUOTE    0xB2
#define OP_BOOT_VERIFY   0xB3
#define OP_BOOT_RESET    0xB4

/* ── Capability Attenuation ──────────────────────────────────────────────── */
#define OP_CAP_ATTENUATE  0x14   /* derive cap with rights ⊆ parent rights */

/*
 * CAP_ATTENUATE(cap_ch, src_handle, new_rights, grantee_pd, revokable)
 *
 * Derive a new capability with reduced rights from an existing cap.
 * Returns the new handle in MR0 (0 on failure).
 *
 * Rights bits (from cap_broker.c):
 *   CAP_RIGHT_READ  = 0x01
 *   CAP_RIGHT_WRITE = 0x02
 *   CAP_RIGHT_GRANT = 0x04
 *   CAP_RIGHT_EXEC  = 0x08
 */
#define CAP_ATTENUATE(cap_ch, src_handle, new_rights, grantee_pd, revokable) \
    (__extension__({ \
        microkit_mr_set(1, (src_handle)); \
        microkit_mr_set(2, (new_rights)); \
        microkit_mr_set(3, (grantee_pd)); \
        microkit_mr_set(4, (revokable) ? 1 : 0); \
        microkit_msginfo _r = PPCALL_DONATE((cap_ch), \
            microkit_msginfo_new(OP_CAP_ATTENUATE, 4), \
            PRIO_CONTROLLER, PRIO_MONITOR); \
        (uint32_t)microkit_mr_get(0); \
    }))

/* Revoke a cap and all its derived (attenuated) children */
#define CAP_REVOKE_TREE(cap_ch, handle) do { \
    microkit_mr_set(1, (handle)); \
    microkit_mr_set(2, 1);  /* cascade=true */ \
    PPCALL_DONATE((cap_ch), microkit_msginfo_new(OP_CAP_REVOKE, 2), \
        PRIO_CONTROLLER, PRIO_MONITOR); \
} while(0)

/* ── Watchdog escalation support ─────────────────────────────────────────── */
/* Additional agent_pool API for mutable access */
agent_entry_t *agent_pool_get_mut(int idx);

/* Map slot index to its IPC channel id (implementation in monitor.c) */
microkit_channel slot_channel(uint32_t slot);

/* Event type for watchdog escalation */
#define MSG_EVENT_WATCHDOG_ESCALATION  0x30

/* ── Fault injection PD ─────────────────────────────────────────────────── */
#define CH_FAULT_INJECT          70   /* monitor <-> fault_inject channel     */
#define OP_FAULT_INJECT          0xF0
#define OP_FAULT_STATUS          0xF1
#define OP_FAULT_RESET           0xF2
#define FAULT_NULL_DEREF         0x01
#define FAULT_STACK_OVF          0x02
#define FAULT_QUOTA_EXCEEDED     0x03
#define FAULT_IPC_TIMEOUT        0x04
#define FAULT_UNALIGNED_MEM      0x05
#define FAULT_FLAG_VERIFY_RECOVERY  0x01
#define FAULT_FLAG_EXPECT_NO_CRASH  0x02
#define FAULT_RESULT_OK          0x00
#define FAULT_RESULT_ERROR       0x01
#define FAULT_RESULT_TIMEOUT     0x02
#define FAULT_RESULT_NO_CRASH    0x03
#define TRACE_EVENT_FAULT_INJECT    0x20
#define TRACE_EVENT_FAULT_RECOVERY  0x21

/* ── Slot status query (used by fault_inject recovery verification) ─────── */
#define MSG_SLOT_STATUS_QUERY    0x50
#define SLOT_STATE_RUNNING       0x01
#define SLOT_STATE_CRASHED       0x02
#define SLOT_STATE_RESTARTING    0x03
