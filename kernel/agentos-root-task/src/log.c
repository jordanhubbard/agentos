/*
 * agentOS Debug Logging
 * 
 * Simple structured logging for the kernel layer PDs.
 log_drain_write(15, 15, "");
 */

#include <stdint.h>
#include "agentos.h"

/*
 * Weak fallback: PDs that don't map the console_rings MR get value 0,
 * causing log_drain_write() to fall back to microkit_dbg_puts.
 * log_drain.c provides the strong definition (with setvar_vaddr).
 */
__attribute__((weak)) uintptr_t log_drain_rings_vaddr = 0;

/* Ultra-minimal hex output (no libc) */
static void log_hex64(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) {
        buf[i] = hex[v & 0xf];
        v >>= 4;
    }
    log_drain_write(15, 15, buf);
}

static void log_u32(uint32_t v) {
    log_drain_write(15, 15, "0");
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    }
    log_drain_write(15, 15, &buf[i]);
}

void agentos_log_boot(const char *pd_name) {
    {
        /* Buffer must hold UTF-8 box-drawing strings: ═ is 3 bytes × 38 = 114 bytes per line.
         * Two border lines (121 bytes each) + two body lines (~45 bytes each) + footer = ~379 bytes.
         * 512 bytes is the minimum safe size. */
        char _cl_buf[512] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "╔══════════════════════════════════════╗\n"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "║           agentOS v0.1.0             ║\n"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "║  The OS for Agents, by Agents        ║\n"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "╚══════════════════════════════════════╝\n"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "[boot] protection domain: "; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = pd_name; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " starting\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(15, 15, _cl_buf);
    }
}

void agentos_log_info(const char *pd, const char *msg) {
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = "["; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = pd; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "] "; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = msg; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(15, 15, _cl_buf);
    }
}

void agentos_log_channel(const char *pd, uint32_t ch) {
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = "["; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = pd; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "] notified on channel "; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(15, 15, _cl_buf);
    }
    log_u32(ch);
    log_drain_write(15, 15, "\n");
}

void agentos_log_fault(const char *pd, agentos_fault_t *f) {
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = "[FAULT] in "; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = pd; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = ": kind="; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(15, 15, _cl_buf);
    }
    log_u32(f->kind);
    log_drain_write(15, 15, " addr=");
    log_hex64(f->addr);
    log_drain_write(15, 15, " ip=");
    log_hex64(f->ip);
    log_drain_write(15, 15, "\n");
}
