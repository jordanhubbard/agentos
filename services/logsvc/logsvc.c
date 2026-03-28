/*
 * agentOS LogSvc — Audit and Logging Service
 *
 * Centralized, capability-audited logging for the entire system.
 * Every capability operation, IPC message, and agent event flows through here.
 *
 * Structured log format (JSON-compatible) for easy agent consumption.
 * Agents can QUERY the log — they can read their own history.
 * The log is the system's memory.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#define LOGSVC_MAX_ENTRIES  16384
#define LOG_MSG_MAX         512
#define LOG_COMPONENT_MAX   64

/* Log levels */
typedef enum {
    LOG_TRACE   = 0,
    LOG_DEBUG   = 1,
    LOG_INFO    = 2,
    LOG_WARN    = 3,
    LOG_ERROR   = 4,
    LOG_FATAL   = 5,
    LOG_AUDIT   = 6,   /* Capability operations — always recorded */
    LOG_EVENT   = 7,   /* System events */
} log_level_t;

static const char *LEVEL_NAMES[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "AUDIT", "EVENT"
};

typedef struct {
    uint64_t    seq;                    /* Monotonic sequence number */
    uint64_t    timestamp_us;           /* Timestamp in microseconds */
    uint8_t     agent[32];              /* AgentID that generated this */
    log_level_t level;
    char        component[LOG_COMPONENT_MAX];
    char        message[LOG_MSG_MAX];
} log_entry_t;

/* Circular log buffer */
static log_entry_t log_buf[LOGSVC_MAX_ENTRIES];
static uint64_t    log_head = 0;      /* Next write position */
static uint64_t    log_seq  = 0;      /* Global sequence counter */

/* Minimum level that gets written (everything at or above) */
static log_level_t min_level = LOG_DEBUG;

int logsvc_init(void) {
    memset(log_buf, 0, sizeof(log_buf));
    log_head = 0;
    log_seq = 0;
    
    printf("[logsvc] LogSvc initialized (ring buffer: %d entries)\n", 
           LOGSVC_MAX_ENTRIES);
    
    /* Write the first log entry */
    logsvc_write(NULL, LOG_INFO, "logsvc", 
                  "agentOS LogSvc initialized. Structured audit logging active.");
    return 0;
}

int logsvc_write(uint8_t *agent, log_level_t level, const char *component,
                  const char *message) {
    if (level < min_level && level != LOG_AUDIT) return 0;
    
    int idx = (int)(log_head % LOGSVC_MAX_ENTRIES);
    log_buf[idx].seq = log_seq++;
    log_buf[idx].timestamp_us = 0; /* TODO: seL4 timer */
    if (agent) memcpy(log_buf[idx].agent, agent, 32);
    else memset(log_buf[idx].agent, 0, 32);
    log_buf[idx].level = level;
    if (component) strncpy(log_buf[idx].component, component, LOG_COMPONENT_MAX - 1);
    if (message) strncpy(log_buf[idx].message, message, LOG_MSG_MAX - 1);
    log_head++;
    
    /* Echo to serial (visible in QEMU console) */
    const char *lvl_name = (level < 8) ? LEVEL_NAMES[level] : "?";
    printf("[%-9s][%-16s] %s\n", lvl_name, component ? component : "?", message);
    
    return 0;
}

int logsvc_writef(uint8_t *agent, log_level_t level, const char *component,
                   const char *fmt, ...) {
    char buf[LOG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return logsvc_write(agent, level, component, buf);
}

/*
 * Query log entries
 * Returns entries with seq >= since_seq, up to max_count
 * Optionally filtered by agent or level
 */
int logsvc_query(uint64_t since_seq, uint8_t *filter_agent, 
                  int filter_level, log_entry_t **out, int *out_count) {
    static log_entry_t results[256];
    int count = 0;
    
    /* Walk the ring buffer from oldest to newest */
    uint64_t start = (log_head > LOGSVC_MAX_ENTRIES) ? 
                      (log_head - LOGSVC_MAX_ENTRIES) : 0;
    
    for (uint64_t i = start; i < log_head && count < 256; i++) {
        int idx = (int)(i % LOGSVC_MAX_ENTRIES);
        
        if (log_buf[idx].seq < since_seq) continue;
        if (filter_level >= 0 && log_buf[idx].level < filter_level) continue;
        if (filter_agent && memcmp(log_buf[idx].agent, filter_agent, 32) != 0) continue;
        
        memcpy(&results[count++], &log_buf[idx], sizeof(log_entry_t));
    }
    
    *out = results;
    *out_count = count;
    return 0;
}

/* Emit log as JSON (for agent consumption) */
int logsvc_entry_to_json(const log_entry_t *entry, char *buf, int buf_size) {
    return snprintf(buf, buf_size,
        "{"
        "\"seq\":%llu,"
        "\"ts_us\":%llu,"
        "\"level\":\"%s\","
        "\"component\":\"%s\","
        "\"message\":\"%s\""
        "}",
        (unsigned long long)entry->seq,
        (unsigned long long)entry->timestamp_us,
        LEVEL_NAMES[entry->level < 8 ? entry->level : 0],
        entry->component,
        entry->message
    );
}
