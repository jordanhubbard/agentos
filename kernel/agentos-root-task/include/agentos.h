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

/* EventBus error codes (returned via msginfo label on error replies) */
#define EVENTBUS_ERR_OVERFLOW  3  /* ring buffer full — event dropped, overflow_count incremented */

/* InitAgent PD channels */
#define INITAGENT_CH_MONITOR  1
#define INITAGENT_CH_EVENTBUS 2

/* Swap Slot PD channels (from swap slot perspective) */
#define SWAPSLOT_CH_CONTROLLER 0

/* Swap Slot channel IDs (from controller perspective) */
#define SWAP_SLOT_BASE_CH     30  /* Channels 30-33 are swap slots (matches agentos.system) */
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

    /* Dynamic agent spawn (caller -> init_agent) */
    MSG_SPAWN_AGENT            = 0x0801,  /* Spawn a WASM agent by hash */
    MSG_SPAWN_AGENT_REPLY      = 0x0802,  /* Reply: agent_id or error */

    /* VibeEngine module registry opcodes (in OP_ space, not MSG_) */
    OP_VIBE_REPLAY             = 0x46,   /* Boot replay: seed registry from AgentFS */
    OP_VIBE_HOTRELOAD          = 0x47,   /* Zero-downtime slot update (was REGISTRY_QUERY) */
    OP_VIBE_REGISTRY_STATUS    = 0x48,   /* Return total registry entries + stats */
    OP_VIBE_REGISTRY_QUERY     = 0x4B,   /* Query registry by hash: known? flags? */

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

    /* mem_profiler messages (0x0C00–0x0CFF) */
    MSG_MEM_LEAK_ALERT         = 0x0C01,  /* mem_profiler -> watchdog: slot leak detected */

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

/* OP_VIBE_HOTRELOAD return codes (MR0) */
#define HOTRELOAD_OK           0x00  /* Hot-reload succeeded */
#define HOTRELOAD_FALLBACK     0x01  /* Layout/caps mismatch — fall back to teardown+respawn */
#define HOTRELOAD_ERR_CAPS     0x02  /* New module requests caps not in slot's grants */

/* Watchdog PD opcodes (controller PPCs into watchdog_pd) */
#define OP_WD_REGISTER         0x50  /* MR1=slot_id, MR2=heartbeat_ticks: start monitoring */
#define OP_WD_HEARTBEAT        0x51  /* MR1=slot_id: update heartbeat tick */
#define OP_WD_STATUS           0x52  /* MR1=slot_id → MR0=status, MR1=ticks_remaining */
#define OP_WD_UNREGISTER       0x53  /* MR1=slot_id: stop monitoring slot */
#define OP_WD_FREEZE           0x54  /* MR1=slot_id: suspend monitoring (hotreload in progress) */
#define OP_WD_RESUME           0x55  /* MR1=slot_id, MR2=new_module_hash_lo: resume + update hash */

/* Watchdog status return codes */
#define WD_OK                  0x00
#define WD_ERR_NOENT           0x01  /* slot_id not registered */
#define WD_ERR_FULL            0x02  /* watchdog slot table full */

/* Watchdog PD channel IDs (from controller perspective) */
#define CH_WATCHDOG_CTRL       56  /* controller PPCs into watchdog_pd (register/freeze/resume) */
#define CH_WATCHDOG_NOTIFY     54  /* watchdog_pd notifies controller on heartbeat timeout */

/* trace_recorder PD opcodes (MR0) */
#define OP_TRACE_START         0x80u  /* begin recording; reset buffer */
#define OP_TRACE_STOP          0x81u  /* stop recording; finalize */
#define OP_TRACE_QUERY         0x82u  /* MR0=event_count, MR1=bytes_used */
#define OP_TRACE_DUMP          0x83u  /* serialize to JSONL in trace_out region */

/* trace_recorder channel IDs (from controller perspective) */
#define CH_TRACE_CTRL           6  /* controller PPCs into trace_recorder (START/STOP/QUERY/DUMP) */
#define CH_TRACE_NOTIFY         7  /* controller notifies trace_recorder on each dispatch */

/*
 * Numeric PD IDs for trace_recorder src_pd / dst_pd fields.
 * Packed into MR0 by controller before notifying CH_TRACE_NOTIFY:
 *   MR0 = (TRACE_PD_* << 24) | (TRACE_PD_* << 16) | label[15:0]
 */
#define TRACE_PD_CONTROLLER    0u
#define TRACE_PD_EVENT_BUS     1u
#define TRACE_PD_INIT_AGENT    2u
#define TRACE_PD_WORKER_0      3u
#define TRACE_PD_WORKER_1      4u
#define TRACE_PD_WORKER_2      5u
#define TRACE_PD_WORKER_3      6u
#define TRACE_PD_WORKER_4      7u
#define TRACE_PD_WORKER_5      8u
#define TRACE_PD_WORKER_6      9u
#define TRACE_PD_WORKER_7     10u
#define TRACE_PD_AGENTFS      11u
#define TRACE_PD_VIBE_ENGINE  12u
#define TRACE_PD_SWAP_SLOT_0  13u
#define TRACE_PD_SWAP_SLOT_1  14u
#define TRACE_PD_SWAP_SLOT_2  15u
#define TRACE_PD_SWAP_SLOT_3  16u
#define TRACE_PD_GPU_SCHED    17u
#define TRACE_PD_MESH_AGENT   18u
#define TRACE_PD_CAP_AUDIT    19u
#define TRACE_PD_FAULT_HDL    20u
#define TRACE_PD_DEBUG_BRIDGE 21u
#define TRACE_PD_QUOTA_PD     22u
#define TRACE_PD_MEM_PROFILER 23u
#define TRACE_PD_WATCHDOG_PD  24u
#define TRACE_PD_TRACE_REC    25u
#define TRACE_PD_NAMESERVER   26u
#define TRACE_PD_VFS_SERVER   27u
#define TRACE_PD_VIRTIO_BLK   28u
#define TRACE_PD_SPAWN_SERVER 29u
#define TRACE_PD_NET_SERVER   30u
#define TRACE_PD_APP_MANAGER  31u
#define TRACE_PD_HTTP_SVC     32u
#define TRACE_PD_APP_SLOT     33u

/* vm_manager IPC opcodes (MR0 in PPCs to vm_manager PD, channel CH_VM_MANAGER)
 * NOTE: 0x15/0x16 are shared with OP_CAP_BROKER_RELOAD/OP_CAP_STATUS but those
 * are dispatched by the monitor/cap_broker PD, not vm_manager — no conflict. */
#define OP_VM_CREATE    0x10u  /* MR1=label_vaddr MR2=ram_mb → MR0=ok MR1=slot_id */
#define OP_VM_DESTROY   0x11u  /* MR1=slot_id → MR0=ok */
#define OP_VM_START     0x12u  /* MR1=slot_id → MR0=ok */
#define OP_VM_STOP      0x13u  /* MR1=slot_id → MR0=ok */
#define OP_VM_PAUSE     0x14u  /* MR1=slot_id → MR0=ok */
#define OP_VM_RESUME    0x15u  /* MR1=slot_id → MR0=ok */
#define OP_VM_CONSOLE   0x16u  /* MR1=slot_id → MR0=ok */
#define OP_VM_INFO      0x17u  /* MR1=slot_id → MR0=ok MR1=state MR2=ram_vaddr */
#define OP_VM_LIST      0x18u  /* → MR0=ok MR1=count; vm_list_shmem has vm_list_entry_t[] */
#define OP_VM_SNAPSHOT  0x19u  /* MR1=slot_id → MR0=ok MR1=snap_hash_lo MR2=snap_hash_hi */
#define OP_VM_RESTORE   0x1Au  /* MR1=slot_id MR2=snap_lo MR3=snap_hi → MR0=ok */

/* Channel IDs for HURD-parity VM management PDs */
#define CH_VM_MANAGER   45u   /* controller PPCs into vm_manager */
#define CH_VM_SNAPSHOT  46u   /* controller PPCs into vm_snapshot (Phase 2) */

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
#define OP_CAP_ATTEST         0x53  /* Generate signed capability attestation report */

/* Quota PD opcodes */
#define OP_QUOTA_REGISTER     0x60  /* Register agent with cpu/mem limits */
#define OP_QUOTA_TICK         0x61  /* Tick agent cpu/mem usage */
#define OP_QUOTA_STATUS       0x62  /* Query agent quota state */

/* cap_policy hot-reload opcodes */
#define OP_CAP_POLICY_RELOAD   0xC0u  /* fetch+parse+validate new policy from AgentFS */
#define OP_CAP_POLICY_STATUS   0xC1u  /* → MR0=loaded, MR1=version, MR2=count, MR3=hash */
#define OP_CAP_POLICY_RESET    0xC2u  /* revert to static compile-time policy */
#define OP_CAP_POLICY_DIFF     0xC3u  /* → MR0=revoked, MR1=classes, MR2=version */

/* cap_policy hot-reload event bus IDs */
#define EVENT_POLICY_RELOADED  0x30u  /* MR2=classes_loaded, MR3=grants_revoked, MR4=version */

/* snapshot_sched PD opcodes (priority 180, passive) */
#define OP_SNAP_STATUS        0xB0u  /* → MR1=rounds, MR2=total_snapped, MR3=tick, MR4=slot_count */
#define OP_SNAP_SET_POLICY    0xB1u  /* MR1=interval_ticks, MR2=min_delta_kb */
#define OP_SNAP_FORCE         0xB2u  /* Force immediate round → MR0=ok, MR1=round# */
#define OP_SNAP_GET_HISTORY   0xB3u  /* → MR1..MR8 = last 4 round summaries */

/* snapshot_sched event bus IDs */
#define EVENT_SNAP_SCHED_DONE 0x20u  /* MR2=slots_checked, MR3=slots_snapped, MR4=tick */

/* snapshot_sched configuration defaults */
#define SNAP_INTERVAL_TICKS_DEFAULT  500u  /* ticks between rounds (~5s @ 100Hz) */
#define SNAP_MIN_DELTA_DEFAULT        64u  /* min heap-KB change to force snap */
#define SNAP_MAX_SLOTS                 8u  /* max simultaneously tracked slots */
#define OP_QUOTA_SET          0x63  /* Update agent quota limits */

/* Fault handler restart policy constants */
#define FAULT_POLICY_MAX_RESTARTS_DEFAULT  3u
#define FAULT_POLICY_RESTART_DELAY_MS      100u
#define FAULT_POLICY_ESCALATE_AFTER        5u

/* fault_handler extended opcodes */
#define OP_FAULT_POLICY_SET   0xE0  /* MR1=slot, MR2=max_restarts, MR3=escalate_after */

/* Quota flags */
#define QUOTA_FLAG_ACTIVE     (1u << 0)
#define QUOTA_FLAG_CPU_EXCEED (1u << 1)
#define QUOTA_FLAG_MEM_EXCEED (1u << 2)
#define QUOTA_FLAG_REVOKED    (1u << 3)

/* Capability event types */
#define CAP_EVENT_GRANT            1
#define CAP_EVENT_REVOKE           2
#define CAP_AUDIT_POLICY_RELOAD    8u  /* policy hot-reload; agent_id=checked, caps_mask=revoked, slot_id=version */

/* Capability Broker opcodes (MR0 for PPCs into monitor / cap_broker dispatch) */
/* NOTE: OP_CAP_BROKER_RELOAD (0x15) is the cap_broker PPC opcode.
 *       OP_CAP_POLICY_RELOAD (0xC0) is the cap_policy PD's own opcode — distinct. */
#define OP_CAP_BROKER_RELOAD  0x15u  /* hot-reload policy blob; revoke violating grants atomically */
#define OP_CAP_STATUS         0x16u  /* query: MR0=cap_count, MR1=policy_version, MR2=active_grants */

/* Capability class bitmask (mirrors WASM capability manifest) */
#define CAP_CLASS_FS          (1 << 0)
#define CAP_CLASS_NET         (1 << 1)
#define CAP_CLASS_GPU         (1 << 2)
#define CAP_CLASS_IPC         (1 << 3)
#define CAP_CLASS_TIMER       (1 << 4)
#define CAP_CLASS_STDIO       (1 << 5)
#define CAP_CLASS_SPAWN       (1 << 6)
#define CAP_CLASS_SWAP        (1 << 7)

/*
 * Fine-grained AGENTOS_CAP_* capability constants.
 * These are the authoritative bitmask values used by cap_broker, cap_policy,
 * and the WASM manifest verification path (verify_capabilities_manifest).
 * They appear as cap_class fields in agentos.system annotations and are
 * the values agents declare in their agentos.capabilities custom section.
 */
#define AGENTOS_CAP_COMPUTE      0x01u  /* CPU time budget */
#define AGENTOS_CAP_MEMORY       0x02u  /* heap allocation */
#define AGENTOS_CAP_OBJECTSTORE  0x04u  /* AgentFS access */
#define AGENTOS_CAP_NETWORK      0x08u  /* network endpoints */
#define AGENTOS_CAP_SPAWN        0x10u  /* spawn child agents */
#define AGENTOS_CAP_AUDIT        0x20u  /* read audit log */
#define AGENTOS_CAP_SWAP_WRITE   0x40u  /* write WASM to swap region (controller only) */
#define AGENTOS_CAP_SWAP_READ    0x80u  /* read/execute WASM from swap region (swap slots) */

/* NameServer channel IDs (from controller perspective)
 * IDs 18-24 reserved for microkernel service layer (Microkit limit: id < 62) */
#define CH_NAMESERVER         18  /* controller -> nameserver (PPC) */

/* Service layer channel IDs (from controller perspective) */
#define CH_VFS_SERVER         19   /* controller -> vfs_server (PPC) */
#define CH_SPAWN_SERVER       20   /* controller -> spawn_server (PPC) */
#define CH_NET_SERVER         21   /* controller -> net_server (PPC) */
#define CH_NET_TIMER          59   /* controller -> net_server (lwIP 10ms tick notify) */
#define CH_VIRTIO_BLK         22   /* controller -> virtio_blk (PPC) */
#define CH_APP_MANAGER        23   /* controller -> app_manager (PPC) */
#define CH_HTTP_SVC           24   /* controller -> http_svc (PPC) */

/* Log Drain channel IDs (from controller perspective) */
#ifdef BOARD_qemu_virt_aarch64
#define CH_LOG_DRAIN          55
#else
#define CH_LOG_DRAIN          60
#endif

/* Log Drain op codes (MR0 in PPC to log_drain) */
#define OP_LOG_WRITE          0x01  /* PD -> log_drain: register ring slot + flush */
#define OP_LOG_STATUS         0x02  /* returns: MR1=slot_count, MR2=bytes_drained */

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
    uint32_t magic;          /* 0xA6E7_0B05 = "AGENTOS" */
    uint32_t version;
    uint64_t capacity;       /* number of event slots */
    uint64_t head;           /* write index */
    uint64_t tail;           /* read index */
    uint32_t overflow_count; /* incremented each time a publish is dropped (ring full) */
    uint8_t  _pad[36];       /* pad to maintain total struct size */
} agentos_ring_header_t;

#define AGENTOS_RING_MAGIC 0xA6E70B05

/* Eventbus ring memory layout (must match memory_region size in *.system files) */
#define EVENTBUS_RING_SIZE            0x40000u  /* 256 KB — matches agentos*.system */
#define EVENTBUS_BATCH_STAGING_SIZE   768u      /* bytes reserved at end for batch publish */
#define EVENTBUS_BATCH_STAGING_OFFSET (EVENTBUS_RING_SIZE - EVENTBUS_BATCH_STAGING_SIZE)

/*
 * OP_PUBLISH_BATCH: coalesce up to 16 MsgBus events in a single seL4_Call.
 *
 * Caller writes packed batch_event_t entries into shared memory starting at
 * byte offset MR1 from the eventbus ring base, then calls with:
 *   label = OP_PUBLISH_BATCH (0x25)
 *   MR0   = event count (1-16)
 *   MR1   = byte offset into shared mem where entries start
 *
 * event_bus iterates all entries, writes each to the ring, notifies all
 * subscribed channels once after the full pass.
 *
 * Returns: MR0 = events_dispatched, MR1 = events_dropped (ring full)
 */
#define OP_PUBLISH_BATCH       0x25u
#define PUBLISH_BATCH_MAX      16u

/*
 * Batch event entry layout (variable-length, 4-byte aligned records).
 * data[] holds: topic_len bytes of topic, then payload_len bytes of payload.
 * The event kind written to the ring is the first ≤4 bytes of topic[]
 * interpreted as a little-endian uint32_t.
 */
typedef struct __attribute__((packed)) {
    uint16_t topic_len;     /* byte length of topic string in data[] */
    uint16_t payload_len;   /* byte length of payload in data[] after topic */
    char     data[];        /* topic bytes followed by payload bytes */
} batch_event_t;

/*
 * Stride to the next batch_event_t entry (4-byte aligned).
 * Caller advances pointer by this many bytes after processing each entry.
 */
#define BATCH_ENTRY_STRIDE(e) \
    (((uint32_t)(sizeof(batch_event_t) + (e)->topic_len + (e)->payload_len) + 3u) & ~3u)

/* ── auth_server opcodes ─────────────────────────────────────────────── */
#define OP_AUTH_LOGIN    0xF0u  /* MR1=uid → MR0=ok, MR1=token_id */
#define OP_AUTH_VERIFY   0xF1u  /* MR1=token_id → MR0=ok, MR1=uid, MR2=cap_mask */
#define OP_AUTH_REVOKE   0xF2u  /* MR1=token_id → MR0=ok */
#define OP_AUTH_ADDUSER  0xF3u  /* MR1=uid, MR2=cap_mask → MR0=ok */
#define OP_AUTH_STATUS   0xF4u  /* → MR0=ok, MR1=active_tokens, MR2=active_users */

/* Channel ID for auth_server (from controller perspective) */
#define CH_AUTH_SERVER      29u
#define CH_PROC_SERVER      27u   /* controller PPCs into proc_server */
#define CH_PFLOCAL_SERVER   26u   /* controller PPCs into pflocal_server */
#define CH_EXEC_SERVER      28u   /* controller PPCs into exec_server */
#define CH_TERM_SERVER      43u   /* controller PPCs into term_server */

/* Trace PD IDs for HURD-parity PDs */
#define TRACE_PD_AUTH_SERVER   34u
#define TRACE_PD_PROC_SERVER   35u
#define TRACE_PD_PFLOCAL       36u
#define TRACE_PD_VM_SNAPSHOT   37u
#define TRACE_PD_EXT2FS_ALT    38u   /* see also TRACE_PD_EXT2FS below */
#define TRACE_PD_EXEC_SERVER   39u
#define TRACE_PD_TERM_SERVER   40u

/* ─── ext2fs Protection Domain ────────────────────────────────────────────
 * Track N: persistent ext2 filesystem over virtio_blk.
 *
 * Channel CH_EXT2FS (47): controller PPCs into ext2fs.
 * TRACE_PD_EXT2FS (38):   trace recorder PD ID for ext2fs.
 *
 * Opcodes (MR0 in PPCs on CH_EXT2FS):
 *   OP_EXT2_MOUNT   — mount filesystem; returns block_count, inode_count
 *   OP_EXT2_STAT    — stat path (path in ext2_shmem); returns inode/size/mode
 *   OP_EXT2_READ    — read file data; MR1=inode MR2=offset MR3=len
 *   OP_EXT2_WRITE   — write (Phase 1: returns EXT2_ERR_READONLY)
 *   OP_EXT2_READDIR — list directory; MR1=dir_inode; entries in ext2_shmem
 *   OP_EXT2_STATUS  — filesystem status; returns mounted/block_count/free_blocks
 * ──────────────────────────────────────────────────────────────────────── */
#define CH_EXT2FS          47u
#define TRACE_PD_EXT2FS    38u

#define OP_EXT2_MOUNT      0x50u  /* → MR0=ok, MR1=block_count, MR2=inode_count */
#define OP_EXT2_STAT       0x51u  /* path in ext2_shmem → MR1=inode, MR2=size_lo, MR3=mode */
#define OP_EXT2_READ       0x52u  /* MR1=inode, MR2=offset, MR3=len → bytes in ext2_shmem, MR1=actual */
#define OP_EXT2_WRITE      0x53u  /* MR1=inode, MR2=offset, MR3=len from ext2_shmem → MR1=written */
#define OP_EXT2_READDIR    0x54u  /* MR1=dir_inode → entries in ext2_shmem, MR1=count */
#define OP_EXT2_STATUS     0x55u  /* → MR0=ok, MR1=mounted, MR2=block_count, MR3=free_blocks */

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

/* ═══════════════════════════════════════════════════════════════════════════
 * Log Drain Ring Buffer — per-PD structured output ring for log_drain
 *
 * Each PD gets a 4KB slot in the log_drain_rings shared memory region.
 * Layout: log_drain_ring_t at offset 0, data bytes follow.
 * Wire format is unchanged from the prior console_mux ring (magic 0xC0DE4D55).
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LOG_DRAIN_RING_MAGIC  0xC0DE4D55
#define LOG_DRAIN_RING_SIZE   0x1000
#define LOG_DRAIN_RING_HDR_SZ 16
#define LOG_DRAIN_DATA_SIZE   (LOG_DRAIN_RING_SIZE - LOG_DRAIN_RING_HDR_SZ)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t pd_id;
    uint32_t head;    /* write offset into data area (PD increments) */
    uint32_t tail;    /* read offset into data area (log_drain increments) */
} log_drain_ring_t;

extern uintptr_t log_drain_rings_vaddr;

static inline void log_drain_write(uint32_t slot, uint32_t pd_id, const char *msg)
{
    if (!log_drain_rings_vaddr) {
        microkit_dbg_puts(msg);
        return;
    }

    volatile log_drain_ring_t *hdr =
        (volatile log_drain_ring_t *)(log_drain_rings_vaddr + slot * LOG_DRAIN_RING_SIZE);

    if (hdr->magic != LOG_DRAIN_RING_MAGIC) {
        hdr->pd_id = pd_id;
        hdr->head  = 0;
        hdr->tail  = 0;
        hdr->magic = LOG_DRAIN_RING_MAGIC;
    }

    volatile uint8_t *data = (volatile uint8_t *)(hdr + 1);
    uint32_t h = hdr->head;

    for (const char *p = msg; *p; p++) {
        uint32_t next = (h + 1) % LOG_DRAIN_DATA_SIZE;
        if (next == hdr->tail) break;
        data[h] = (uint8_t)*p;
        h = next;
    }
    hdr->head = h;

    microkit_mr_set(0, OP_LOG_WRITE);
    microkit_mr_set(1, slot);
    microkit_mr_set(2, pd_id);
    microkit_ppcall(CH_LOG_DRAIN, microkit_msginfo_new(OP_LOG_WRITE, 2));
}

/* ─── proc_server Protection Domain ───────────────────────────────────────
 * Track F: HURD proc server — process table, signals, and lifecycle.
 * Channel CH_PROC_SERVER (27): controller PPCs into proc_server.
 */
#define OP_PROC_SPAWN    0xC0u  /* MR1=parent_pid MR2=auth_token MR3=cap_mask → ok+pid */
#define OP_PROC_EXIT     0xC1u  /* MR1=pid MR2=exit_code → ok */
#define OP_PROC_WAIT     0xC2u  /* MR1=pid → ok+exit_code+state */
#define OP_PROC_STATUS   0xC3u  /* MR1=pid → ok+state+cap_mask */
#define OP_PROC_LIST     0xC4u  /* → ok+count; proc_shmem has proc_info_t[] */
#define OP_PROC_KILL     0xC5u  /* MR1=pid MR2=signal → ok */
#define OP_PROC_SETCAP   0xC6u  /* MR1=pid MR2=cap_mask → ok */

/* ─── vm_snapshot Protection Domain ───────────────────────────────────────
 * Track J: VM state save/restore to AgentFS.
 * Channel CH_VM_SNAPSHOT (46): controller PPCs into vm_snapshot.
 * (OP_VM_SNAPSHOT=0x19 and OP_VM_RESTORE=0x1A are in the vm_manager section above)
 */

/* ─── pflocal_server Protection Domain ────────────────────────────────────
 * Track G: AF_UNIX socket emulation via shared memory rings.
 * Channel CH_PFLOCAL_SERVER (26): controller PPCs into pflocal_server.
 */
#define OP_PFLOCAL_SOCKET    0xD0u  /* → ok+sock_id+slot_offset */
#define OP_PFLOCAL_BIND      0xD1u  /* MR1=sock_id, path in shmem → ok */
#define OP_PFLOCAL_LISTEN    0xD2u  /* MR1=sock_id → ok */
#define OP_PFLOCAL_CONNECT   0xD3u  /* MR1=sock_id, path in shmem → ok */
#define OP_PFLOCAL_ACCEPT    0xD4u  /* MR1=sock_id → ok+new_sock_id+peer_slot */
#define OP_PFLOCAL_SEND      0xD5u  /* MR1=sock_id MR2=offset MR3=len → ok+sent */
#define OP_PFLOCAL_RECV      0xD6u  /* MR1=sock_id MR2=offset MR3=max → ok+recv */
#define OP_PFLOCAL_CLOSE     0xD7u  /* MR1=sock_id → ok */
#define OP_PFLOCAL_STATUS    0xD8u  /* MR1=sock_id → ok+state+peer_id */

/* ─── exec_server Protection Domain ───────────────────────────────────────
 * Track H: HURD exec server — ELF binary dispatch to app_slot PDs.
 * Channel CH_EXEC_SERVER (28): controller PPCs into exec_server.
 */
#define OP_EXEC_LAUNCH   0xE0u  /* path in shmem, MR1=auth_token MR2=cap_mask → ok+exec_id */
#define OP_EXEC_STATUS   0xE1u  /* MR1=exec_id → ok+state+pid */
#define OP_EXEC_WAIT     0xE2u  /* MR1=exec_id → ok+pid */
#define OP_EXEC_KILL     0xE3u  /* MR1=exec_id → ok */

/* ─── term_server Protection Domain ───────────────────────────────────────
 * Track I: PTY multiplexer with line discipline.
 * Channel CH_TERM_SERVER (43): controller PPCs into term_server.
 */
#define OP_TERM_OPENPTY    0xA0u  /* → ok+pty_id+master_slot+slave_slot */
#define OP_TERM_RESIZE     0xA1u  /* MR1=pty_id MR2=rows MR3=cols → ok */
#define OP_TERM_WRITE      0xA2u  /* MR1=pty_id MR2=offset MR3=len → ok */
#define OP_TERM_READ       0xA3u  /* MR1=pty_id MR2=offset MR3=max → ok+len */
#define OP_TERM_CLOSEPTY   0xA4u  /* MR1=pty_id → ok */
#define OP_TERM_STATUS     0xA5u  /* → ok+active_ptys */

