/*
 * services/vibeos/vibeos_main.c — vibeOS VOS lifecycle PD entry point
 *
 * Receives seL4 IPC from the root task and dispatches VOS operations
 * (CREATE, DESTROY, SNAPSHOT, RESTORE, STATUS, LIST, ATTACH, DETACH,
 * CONFIGURE, MIGRATE) to the implementation modules:
 *
 *   vos_create.c   — VOS_CREATE, VOS_DESTROY capability delegation
 *   vos_snapshot.c — VOS_SNAPSHOT guest state capture
 *   vos_restore.c  — VOS_RESTORE guest state replay
 *
 * The IPC dispatch loop is not yet implemented; this stub spins until
 * the full vibeos seL4 IPC server is wired in.
 *
 * Contract: contracts/vibeos/interface.h
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>

__attribute__((noreturn)) void pd_main(void)
{
    for (;;) { __asm__ volatile (""); }
}
