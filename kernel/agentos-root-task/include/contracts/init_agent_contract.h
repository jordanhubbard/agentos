/*
 * InitAgent IPC Contract
 *
 * The InitAgent PD orchestrates the agentOS boot sequence and manages
 * the top-level agent lifecycle.
 *
 * Channel: INITAGENT_CH_* (see agentos.h)
 * Opcodes: MSG_INITAGENT_* (see agentos.h)
 *
 * Invariants:
 *   - MSG_INITAGENT_START is sent exactly once by monitor at boot.
 *   - MSG_INITAGENT_READY is sent exactly once by init_agent to monitor.
 *   - MSG_INITAGENT_AGENT_LIST returns data in the shared shmem region;
 *     the caller must hold a mapping to that region.
 *   - SHUTDOWN triggers graceful teardown; all agents are notified before
 *     the reply is sent.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs (InitAgent perspective) ────────────────────────────────── */
#define INITAGENT_CH_MONITOR   1  /* monitor → init_agent */
#define INITAGENT_CH_EVENTBUS  2  /* init_agent → eventbus */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct initagent_req_start {
    uint32_t boot_flags;        /* BOOT_FLAG_* bitmask */
};

#define BOOT_FLAG_RECOVERY  (1u << 0)  /* boot into recovery mode */
#define BOOT_FLAG_VERBOSE   (1u << 1)  /* enable verbose boot logging */

struct initagent_req_shutdown {
    uint32_t reason;            /* SHUTDOWN_REASON_* */
    uint32_t timeout_ms;        /* max ms to wait for agent teardown */
};

#define SHUTDOWN_REASON_HALT    0
#define SHUTDOWN_REASON_REBOOT  1
#define SHUTDOWN_REASON_PANIC   2

struct initagent_req_status {
    /* no fields */
};

struct initagent_req_agent_list {
    /* no fields — results in shmem */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct initagent_reply_start {
    uint32_t ok;
};

struct initagent_reply_shutdown {
    uint32_t ok;
    uint32_t agents_stopped;
};

struct initagent_reply_status {
    uint32_t state;             /* INITAGENT_STATE_* */
    uint32_t agent_count;       /* active agents */
    uint32_t uptime_ticks;
};

#define INITAGENT_STATE_BOOTING   0
#define INITAGENT_STATE_RUNNING   1
#define INITAGENT_STATE_STOPPING  2

struct initagent_reply_agent_list {
    uint32_t count;             /* entries written to shmem */
};

/* ─── Shmem layout: agent_list entry ────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t agent_id;
    uint32_t pd_id;
    uint32_t state;             /* 0=idle 1=running 2=faulted */
    uint32_t cap_mask;
} agent_list_entry_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum initagent_error {
    INITAGENT_OK              = 0,
    INITAGENT_ERR_ALREADY_STARTED = 1,
    INITAGENT_ERR_NOT_STARTED = 2,
    INITAGENT_ERR_SHUTDOWN_TIMEOUT = 3,
};
