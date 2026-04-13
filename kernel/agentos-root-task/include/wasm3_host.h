/*
 * agentOS WASM3 Host — Integration layer for WASM execution in swap slots
 *
 * This bridges wasm3 (WASM interpreter) with the agentOS swap slot PD.
 * It provides:
 *   - WASM module loading from shared memory
 *   - Host import bindings (aos_log, aos_time_us, aos_mem_read/write)
 *   - Export calling (init, handle_ppc, health_check)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef wasm3_host_h
#define wasm3_host_h

#include <stdint.h>
#include <stdbool.h>

/* Result of a WASM PPC handler call */
typedef struct {
    uint64_t label;
    uint64_t mr0;
    uint64_t mr1;
    uint64_t mr2;
    uint64_t mr3;
} wasm_ppc_result_t;

/* WASM runtime state (opaque to swap_slot.c) */
typedef struct wasm3_host wasm3_host_t;

/*
 * Initialize the WASM runtime.
 *
 * @param wasm_bytes   Pointer to the WASM binary (after swap_slot_header_t)
 * @param wasm_size    Size of the WASM binary in bytes
 * @return             Host handle, or NULL on failure
 */
wasm3_host_t *wasm3_host_init(const uint8_t *wasm_bytes, uint32_t wasm_size);

/*
 * Call the WASM module's init() export.
 *
 * @param host  Host handle from wasm3_host_init
 * @return      true if init succeeded
 */
bool wasm3_host_call_init(wasm3_host_t *host);

/*
 * Call the WASM module's health_check() export.
 *
 * @param host  Host handle
 * @return      true if health check passed (WASM returned nonzero)
 */
bool wasm3_host_call_health_check(wasm3_host_t *host);

/*
 * wasm3_host_get_heap_stats() — query refcount_gc live allocations from
 * the WASM module's __agentos_heap_stats export (if present).
 * Returns false if the module was compiled without refcount_gc.h.
 */
bool wasm3_host_get_heap_stats(wasm3_host_t *host,
                                uint32_t     *live_allocs_out,
                                uint32_t     *live_bytes_out);

/*
 * Call the WASM module's handle_ppc() export.
 * 
 * The WASM function signature is:
 *   handle_ppc(label: i64, mr0: i64, mr1: i64, mr2: i64, mr3: i64)
 *
 * Returns are written to a shared memory result buffer at a known offset
 * (since WASM multi-value returns are complex). The WASM module writes:
 *   offset 0: label (i64)
 *   offset 8: mr0 (i64)
 *   offset 16: mr1 (i64)
 *   offset 24: mr2 (i64)
 *   offset 32: mr3 (i64)
 *
 * @param host    Host handle
 * @param label   IPC label
 * @param mr0-3   Message registers
 * @param result  Output: result from WASM handler
 * @return        true if call succeeded
 */
bool wasm3_host_call_ppc(wasm3_host_t *host,
                          uint64_t label, uint64_t mr0, uint64_t mr1,
                          uint64_t mr2, uint64_t mr3,
                          wasm_ppc_result_t *result);

/*
 * Tear down the WASM runtime (frees from fixed heap — mostly a no-op
 * with bump allocator, but resets state for next load).
 */
void wasm3_host_destroy(wasm3_host_t *host);

/*
 * Reset the wasm3 fixed-heap bump allocator back to its initial state.
 * Must be called before wasm3_host_init() when a swap slot PD has been
 * restarted by the seL4 fault handler — the fault clears no BSS, so the
 * heap pointer stays wherever the previous (failed) run left it.
 * Safe to call even on the very first load attempt.
 */
void wasm3_heap_reset(void);

#endif /* wasm3_host_h */
