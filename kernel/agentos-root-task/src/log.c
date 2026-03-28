/*
 * agentOS Debug Logging
 * 
 * Simple structured logging for the kernel layer PDs.
 * Uses microkit_dbg_puts() which goes to the seL4 debug UART.
 */

#include <microkit.h>
#include <stdint.h>
#include "agentos.h"

/* Ultra-minimal hex output (no libc) */
static void log_hex64(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[18] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) {
        buf[i] = hex[v & 0xf];
        v >>= 4;
    }
    microkit_dbg_puts(buf);
}

static void log_u32(uint32_t v) {
    if (v == 0) { microkit_dbg_puts("0"); return; }
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    }
    microkit_dbg_puts(&buf[i]);
}

void agentos_log_boot(const char *pd_name) {
    microkit_dbg_puts("\n");
    microkit_dbg_puts("╔══════════════════════════════════════╗\n");
    microkit_dbg_puts("║           agentOS v0.1.0             ║\n");
    microkit_dbg_puts("║  The OS for Agents, by Agents        ║\n");
    microkit_dbg_puts("╚══════════════════════════════════════╝\n");
    microkit_dbg_puts("[boot] protection domain: ");
    microkit_dbg_puts(pd_name);
    microkit_dbg_puts(" starting\n");
}

void agentos_log_info(const char *pd, const char *msg) {
    microkit_dbg_puts("[");
    microkit_dbg_puts(pd);
    microkit_dbg_puts("] ");
    microkit_dbg_puts(msg);
    microkit_dbg_puts("\n");
}

void agentos_log_channel(const char *pd, uint32_t ch) {
    microkit_dbg_puts("[");
    microkit_dbg_puts(pd);
    microkit_dbg_puts("] notified on channel ");
    log_u32(ch);
    microkit_dbg_puts("\n");
}

void agentos_log_fault(const char *pd, agentos_fault_t *f) {
    microkit_dbg_puts("[FAULT] in ");
    microkit_dbg_puts(pd);
    microkit_dbg_puts(": kind=");
    log_u32(f->kind);
    microkit_dbg_puts(" addr=");
    log_hex64(f->addr);
    microkit_dbg_puts(" ip=");
    log_hex64(f->ip);
    microkit_dbg_puts("\n");
}
