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

    /* AgentFS event types (published to EventBus) */
    MSG_EVENT_PUBLISH          = 0x0410,  /* generic EventBus publish op */
    EVT_OBJECT_CREATED         = 0x0411,  /* new object stored */
    EVT_OBJECT_DELETED         = 0x0412,  /* object tombstoned */
    EVT_OBJECT_EVICTED         = 0x0413,  /* object moved to cold tier */

    /* Worker <-> Controller task ops */
    MSG_WORKER_RETRIEVE        = 0x0701,  /* Worker asks controller to GET from AgentFS */
    MSG_WORKER_RETRIEVE_REPLY  = 0x0702,  /* Controller returns AgentFS data to worker */
    MSG_DEMO_TASK_RETRIEVE     = 0x0710,  /* Task type: retrieve object from AgentFS */

    /* Dynamic agent spawn (caller -> init_agent) */
    MSG_SPAWN_AGENT            = 0x0801,  /* Spawn a WASM agent by hash */
    MSG_SPAWN_AGENT_REPLY      = 0x0802,  /* Reply: agent_id or error */

    /* VibeEngine module registry opcodes (in OP_ space, not MSG_) */
    OP_VIBE_REPLAY             = 0x46,   /* Boot replay: seed registry from AgentFS */
    OP_VIBE_REGISTRY_QUERY     = 0x47,   /* Query registry by hash: known? flags? */
    OP_VIBE_REGISTRY_STATUS    = 0x48,   /* Return total registry entries + stats */

    /* Distributed Agent Mesh (mesh_agent PD) */
    MSG_MESH_ANNOUNCE          = 0x0A01,  /* Node registration: node_id, slot_count, gpu_slots */
    MSG_MESH_ANNOUNCE_REPLY    = 0x0A02,
    MSG_MESH_STATUS            = 0x0A03,  /* Query: number of known peers, total slots */
    MSG_MESH_STATUS_REPLY      = 0x0A04,
    MSG_REMOTE_SPAWN           = 0x0A05,  /* Spawn agent on best-available peer node */
    MSG_REMOTE_SPAWN_REPLY     = 0x0A06,  /* Reply: node_id + ticket_id, or local fallback */
    MSG_MESH_HEARTBEAT         = 0x0A07,  /* Periodic liveness ping from peer */
    MSG_MESH_PEER_DOWN         = 0x0A08,  /* EventBus: peer went offline */

    /* GPU Scheduler PD (agents -> gpu_sched) */
    MSG_GPU_SUBMIT             = 0x0901,  /* Submit GPU task: hash_lo, hash_hi, priority, flags */
    MSG_GPU_SUBMIT_REPLY       = 0x0902,  /* Reply: ticket_id or error */
    MSG_GPU_STATUS             = 0x0903,  /* Query scheduler state */
    MSG_GPU_STATUS_REPLY       = 0x0904,  /* Reply: queue_depth, busy_slots, idle_slots */
    MSG_GPU_CANCEL             = 0x0905,  /* Cancel pending ticket by ticket_id */
    MSG_GPU_CANCEL_REPLY       = 0x0906,
    MSG_GPU_COMPLETE           = 0x0910,  /* EventBus event: task completed */
    MSG_GPU_FAILED             = 0x0911,  /* EventBus event: task failed */

    /* Quota PD (per-agent resource quota enforcement) */
    MSG_QUOTA_REVOKE           = 0x0B01,  /* quota_pd -> controller: revoke agent caps */

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

/* Capability Audit Log channel IDs */
#define CH_CAP_AUDIT_CTRL     57  /* controller -> cap_audit_log (PPC) */
#define CH_CAP_AUDIT_INIT     5   /* init_agent -> cap_audit_log (PPC, from init_agent perspective) */

/* Quota PD channel IDs */
#define CH_QUOTA_CTRL         52  /* controller -> quota_pd (PPC) */
#define CH_QUOTA_INIT         7   /* init_agent -> quota_pd (PPC, from init_agent perspective) */
#define CH_QUOTA_NOTIFY       58  /* controller receives quota revoke notifications */

/* Capability Audit Log opcodes */
#define OP_CAP_LOG            0x50  /* Log grant/revoke event */
#define OP_CAP_LOG_STATUS     0x51  /* Query ring buffer status */
#define OP_CAP_LOG_DUMP       0x52  /* Read entries from ring */

/* Quota PD opcodes */
#define OP_QUOTA_REGISTER     0x60  /* Register agent with cpu/mem limits */
#define OP_QUOTA_TICK         0x61  /* Tick agent cpu/mem usage */
#define OP_QUOTA_STATUS       0x62  /* Query agent quota state */
#define OP_QUOTA_SET          0x63  /* Update agent quota limits */

/* Quota flags */
#define QUOTA_FLAG_ACTIVE     (1u << 0)
#define QUOTA_FLAG_CPU_EXCEED (1u << 1)
#define QUOTA_FLAG_MEM_EXCEED (1u << 2)
#define QUOTA_FLAG_REVOKED    (1u << 3)

/* Capability event types */
#define CAP_EVENT_GRANT       1
#define CAP_EVENT_REVOKE      2

/* Capability class bitmask (mirrors WASM capability manifest) */
#define CAP_CLASS_FS          (1 << 0)
#define CAP_CLASS_NET         (1 << 1)
#define CAP_CLASS_GPU         (1 << 2)
#define CAP_CLASS_IPC         (1 << 3)
#define CAP_CLASS_TIMER       (1 << 4)
#define CAP_CLASS_STDIO       (1 << 5)
#define CAP_CLASS_SPAWN       (1 << 6)
#define CAP_CLASS_SWAP        (1 << 7)

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
