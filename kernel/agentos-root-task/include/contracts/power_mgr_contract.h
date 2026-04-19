#pragma once
/* POWER_MGR contract — version 1
 * PD: power_mgr | Source: src/power_mgr.c | Channel: (platform-specific; ARM PSCI via SMC or HVC)
 */
#include <stdint.h>
#include <stdbool.h>

#define POWER_MGR_CONTRACT_VERSION 1

/* ── Opcodes ── */
#define POWER_MGR_OP_STATUS        0xE200u  /* query system power state and thermal readings */
#define POWER_MGR_OP_DVFS_SET      0xE201u  /* set DVFS operating point for a CPU cluster */
#define POWER_MGR_OP_SLEEP         0xE202u  /* enter system or core sleep state */
#define POWER_MGR_OP_WAKE          0xE203u  /* bring a core or cluster out of sleep */
#define POWER_MGR_OP_HOTPLUG       0xE204u  /* online or offline a CPU core */
#define POWER_MGR_OP_THERMAL_LIMIT 0xE205u  /* set thermal throttle threshold */

/* ── DVFS operating points ── */
#define POWER_DVFS_PERF            0u  /* maximum frequency, maximum voltage */
#define POWER_DVFS_BALANCED        1u  /* balanced frequency/voltage */
#define POWER_DVFS_POWERSAVE       2u  /* minimum frequency, minimum voltage */
#define POWER_DVFS_TURBO           3u  /* boost above nominal (if supported) */

/* ── Sleep states ── */
#define POWER_SLEEP_CORE_IDLE      0u  /* single core WFI (wait-for-interrupt) */
#define POWER_SLEEP_CLUSTER_OFF    1u  /* entire CPU cluster power gated */
#define POWER_SLEEP_SYSTEM_SUSPEND 2u  /* full system suspend to RAM */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* POWER_MGR_OP_STATUS */
} power_mgr_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t cpu_freq_mhz;    /* current CPU frequency in MHz */
    uint32_t dvfs_point;      /* POWER_DVFS_* currently active */
    uint32_t core_count;      /* number of online CPU cores */
    uint32_t temp_millic;     /* die temperature in milli-Celsius */
    uint32_t throttling;      /* 1 if thermal throttling is active */
    uint64_t uptime_ns;       /* system uptime in nanoseconds */
    uint32_t sleep_count;     /* total sleep/wake cycles since boot */
} power_mgr_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* POWER_MGR_OP_DVFS_SET */
    uint32_t cluster_id;      /* CPU cluster index (0 = primary) */
    uint32_t dvfs_point;      /* POWER_DVFS_* target */
    uint32_t freq_mhz;        /* exact frequency (0 = use DVFS preset) */
} power_mgr_req_dvfs_set_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else power_mgr_error_t */
    uint32_t actual_freq_mhz; /* frequency actually applied */
} power_mgr_reply_dvfs_set_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* POWER_MGR_OP_SLEEP */
    uint32_t core_id;         /* target core (0xFFFFFFFF = system-wide) */
    uint32_t sleep_state;     /* POWER_SLEEP_* */
    uint64_t wakeup_ns;       /* wakeup timer (0 = no timeout; wake on interrupt only) */
} power_mgr_req_sleep_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok; returned after wakeup */
    uint32_t wakeup_reason;   /* POWER_WAKEUP_* */
} power_mgr_reply_sleep_t;

#define POWER_WAKEUP_TIMER         0u  /* woken by wakeup_ns timer */
#define POWER_WAKEUP_INTERRUPT     1u  /* woken by external interrupt */
#define POWER_WAKEUP_IPI           2u  /* woken by inter-processor interrupt */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* POWER_MGR_OP_HOTPLUG */
    uint32_t core_id;         /* CPU core to bring online or offline */
    uint32_t online;          /* 1 = bring online, 0 = take offline */
} power_mgr_req_hotplug_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t online_cores;    /* total online cores after operation */
} power_mgr_reply_hotplug_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* POWER_MGR_OP_THERMAL_LIMIT */
    uint32_t throttle_temp_millic;  /* temperature to start throttling (milli-Celsius) */
    uint32_t critical_temp_millic;  /* temperature to force POWERSAVE DVFS */
    uint32_t shutdown_temp_millic;  /* temperature to initiate emergency shutdown */
} power_mgr_req_thermal_limit_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} power_mgr_reply_thermal_limit_t;

/* ── Error codes ── */
typedef enum {
    POWER_MGR_OK              = 0,
    POWER_MGR_ERR_BAD_CLUSTER = 1,  /* cluster_id out of range */
    POWER_MGR_ERR_BAD_CORE    = 2,  /* core_id out of range */
    POWER_MGR_ERR_UNSUPPORTED = 3,  /* DVFS point or sleep state not supported on this HW */
    POWER_MGR_ERR_LAST_CORE   = 4,  /* cannot offline last active core */
    POWER_MGR_ERR_NO_CAP      = 5,  /* caller lacks privilege */
    POWER_MGR_ERR_THROTTLED   = 6,  /* DVFS_SET rejected due to active thermal throttle */
} power_mgr_error_t;

/* ── Invariants ──
 * - power_mgr issues PSCI calls via SMC/HVC; these go through the hypervisor (if present).
 * - At least one CPU core must remain online at all times; HOTPLUG offline of last core is rejected.
 * - POWER_SLEEP_SYSTEM_SUSPEND requires all non-essential PDs to have checkpointed state.
 * - Thermal thresholds must satisfy: throttle < critical < shutdown.
 * - DVFS_SET with freq_mhz=0 uses the nearest supported frequency for the chosen preset.
 * - Only the controller (Ring 1) may invoke power_mgr; agents have no power management access.
 */
