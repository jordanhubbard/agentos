/*
 * sel4_debug_putchar_compat.h
 *
 * libvmm's utility printf backend calls seL4_DebugPutChar directly. The
 * bundled release Microkit SDK only declares that helper when CONFIG_PRINTING
 * is enabled.  agentOS VMM PDs provide microkit_dbg_putc/puts shims backed by
 * their debug UART mapping, so release builds route libvmm printf there.
 */

#pragma once

#ifndef __ASSEMBLER__
#include <sel4/sel4.h>

#ifndef CONFIG_PRINTING
void microkit_dbg_putc(char c);
void microkit_dbg_puts(const char *s);
#define seL4_DebugPutChar(c) microkit_dbg_putc((char)(c))
#define seL4_DebugPutString(s) microkit_dbg_puts((s))
#endif
#endif
