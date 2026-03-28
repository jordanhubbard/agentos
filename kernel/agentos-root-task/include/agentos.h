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

    /* Vibe Swap (VibeEngine -> Controller -> Swap Slots) */
    MSG_VIBE_SWAP_BEGIN        = 0x0501,  /* VibeEngine -> Controller: start swap */
    MSG_VIBE_SWAP_ACTIVATE     = 0x0502,  /* Controller -> slot: go live */
    MSG_VIBE_SWAP_ROLLBACK     = 0x0503,  /* Controller: revert to previous */
    MSG_VIBE_SWAP_HEALTH       = 0x0504,  /* Controller -> slot: health check */
    MSG_VIBE_SWAP_STATUS       = 0x0505,  /* Query swap status */
    MSG_VIBE_SLOT_READY        = 0x0601,  /* Swap slot -> Controller: loaded OK */
    MSG_VIBE_SLOT_FAILED       = 0x0602,  /* Swap slot -> Controller: load failed */
    MSG_VIBE_SLOT_HEALTHY      = 0x0603,  /* Swap slot -> Controller: health OK */

} agentos_msg_tag_t;

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
