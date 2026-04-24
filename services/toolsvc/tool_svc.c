/*
 * services/toolsvc/tool_svc.c — Tool Registry Service stub
 *
 * Stub implementation. Returns SEL4_ERR_NOT_SUPPORTED to all callers.
 * Full implementation: contracts/toolsvc/interface.h
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>

__attribute__((noreturn)) void pd_main(void)
{
    for (;;) { __asm__ volatile (""); }
}
