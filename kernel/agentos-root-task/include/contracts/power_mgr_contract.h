/*
 * PowerMgr IPC Contract
 *
 * The PowerMgr PD controls CPU frequency scaling (DVFS) and system sleep
 * states.  It exposes a simple API for power policy without requiring
 * any PD to directly access power management hardware.
 *
 * Channel: CH_POWER_MGR (see agentos.h)
 * Opcodes: MSG_PWR_STATUS, MSG_PWR_DVFS_SET, MSG_PWR_SLEEP
 *
 * Invariants:
 *   - MSG_PWR_STATUS is read-only.
 *   - MSG_PWR_DVFS_SET validates the requested frequency against the
 *     platform's OPP (operating point) table; illegal values are rejected.
 *   - MSG_PWR_SLEEP level 0 = CPU idle hint (seL4 WFI), level 1 = suspend
 *     (DRAM self-refresh), level 2 = system off (requires wake source config).
 *   - Level 2 sleep is irreversible from the calling PD's perspective;
 *     the system will reboot on wake.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define POWERMGR_CH_CONTROLLER  CH_POWER_MGR

/* ─── Request structs ────────────────────────────────────────────────────── */

struct pwr_req_status {
    /* no fields */
};

struct pwr_req_dvfs_set {
    uint32_t freq_mhz;          /* target CPU frequency in MHz */
    uint32_t voltage_mv;        /* target core voltage in millivolts (0 = auto) */
};

struct pwr_req_sleep {
    uint32_t level;             /* 0=idle 1=suspend 2=off */
    uint32_t wake_source;       /* platform-specific wake source bitmask */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct pwr_reply_status {
    uint32_t ok;
    uint32_t state;             /* PWR_STATE_* */
    uint32_t cur_freq_mhz;
    uint32_t voltage_mv;
    uint32_t thermal_temp_c;    /* 0xFFFFFFFF if sensor unavailable */
};

#define PWR_STATE_RUNNING  0
#define PWR_STATE_IDLE     1
#define PWR_STATE_SUSPEND  2

struct pwr_reply_dvfs_set {
    uint32_t ok;
    uint32_t actual_freq_mhz;   /* closest supported OPP */
    uint32_t actual_voltage_mv;
};

struct pwr_reply_sleep {
    uint32_t ok;                /* only returned for level 0/1; level 2 does not reply */
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum power_mgr_error {
    PWR_OK                  = 0,
    PWR_ERR_BAD_FREQ        = 1,  /* freq_mhz not in OPP table */
    PWR_ERR_BAD_VOLTAGE     = 2,
    PWR_ERR_BAD_LEVEL       = 3,
    PWR_ERR_NO_HW           = 4,  /* DVFS hardware not available */
};
