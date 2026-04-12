/*
 * agentOS Service ABI — canonical interface for hot-swappable services
 *
 * Every service loaded by the vibe hot-swap mechanism MUST implement
 * the three functions below. They are the contract between the kernel's
 * swap slot machinery and the service implementation.
 *
 * Build command (for WASM target):
 *   clang --target=wasm32-wasi -O2 -nostdlib -Wl,--no-entry -Wl,--export-all \
 *         -include agentos_service_abi.h -o service.wasm service.c
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef AGENTOS_SERVICE_ABI_H
#define AGENTOS_SERVICE_ABI_H

#include <stdint.h>
#include <stddef.h>

/* ── Message register access (provided by WASM runtime) ─────────────────── */
extern uint32_t aos_mr_get(int idx);
extern void     aos_mr_set(int idx, uint32_t val);
extern void     aos_log_str(const char *s, int len);

/* ── IPC label constants ─────────────────────────────────────────────────── */
#define AOS_LABEL_INIT      0x0000u  /* Sent once at service startup         */
#define AOS_LABEL_HEALTH    0xFFFFu  /* Health probe — must reply 0          */

/* ── Service ID constants (matches vibe_swap.c SVC_* defines) ───────────── */
#define AOS_SVC_EVENTBUS    0u
#define AOS_SVC_MEMFS       1u
#define AOS_SVC_TOOLSVC     2u
#define AOS_SVC_MODELSVC    3u
#define AOS_SVC_AGENTFS     4u
#define AOS_SVC_LOGSVC      5u

/* ── storage.v1 (MemFS replacement) IPC labels ───────────────────────────── */
#define STORAGE_OP_WRITE    0x30u   /* MR0=op MR1=path_ptr MR2=path_len MR3=data_ptr */
#define STORAGE_OP_READ     0x31u   /* MR0=op MR1=path_ptr MR2=path_len → MR0=data_ptr MR1=len */
#define STORAGE_OP_DELETE   0x32u   /* MR0=op MR1=path_ptr MR2=path_len */
#define STORAGE_OP_STAT     0x33u   /* MR0=op MR1=path_ptr MR2=path_len → MR0=size MR1=flags */
#define STORAGE_OP_LIST     0x34u   /* MR0=op MR1=prefix_ptr MR2=prefix_len → MR0=entries_ptr MR1=count */
#define STORAGE_OP_STAT_SVC 0x20u   /* MR0=op → MR1=file_count MR2=total_bytes */

/* ── Mandatory exports (service MUST implement all three) ────────────────── */

/**
 * service_init — called once when the slot is loaded.
 * Returns 0 on success, negative errno on failure (prevents activation).
 */
int service_init(void);

/**
 * service_dispatch — called for every IPC message directed at this service.
 *
 * @param label      IPC label (operation code)
 * @param in_count   Number of valid input message registers
 * @param out_count  Number of output message registers caller expects
 * @return 0 on success, negative errno on error
 *
 * Read inputs with aos_mr_get(0..in_count-1).
 * Write outputs with aos_mr_set(0..out_count-1).
 */
int service_dispatch(uint32_t label, uint32_t in_count, uint32_t out_count);

/**
 * service_health — called periodically by the swap manager.
 * Returns 0 if healthy, non-zero triggers automatic rollback.
 */
int service_health(void);

#endif /* AGENTOS_SERVICE_ABI_H */
