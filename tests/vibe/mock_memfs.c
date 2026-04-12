/*
 * mock_memfs.c — minimal agentOS storage.v1 conformance test service
 *
 * Used by the vibe integration test as a known-good WASM module.
 * Tests all required STORAGE_OP_* labels and verifies the swap
 * infrastructure works end-to-end without requiring a real LLM.
 *
 * Implements an in-memory flat file table with 64 slots, each holding
 * a 256-byte key and a 4096-byte value.  All message-register access goes
 * through the agentOS WASM ABI (aos_mr_get / aos_mr_set).
 *
 * Compile with:
 *   clang --target=wasm32-wasi -O2 -nostdlib \
 *         -Wl,--no-entry -Wl,--export-all \
 *         -o mock_memfs.wasm mock_memfs.c
 *
 * The resulting WASM binary should have a valid WebAssembly magic header
 * (0x00 0x61 0x73 0x6D) and export service_init, service_dispatch, and
 * service_health.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <stddef.h>

/* ── agentOS WASM runtime imports ────────────────────────────────────────── */

/*
 * Message registers: provided by the agentOS WASM runtime.
 * Do not implement these — they are resolved by the host at load time.
 */
extern uint32_t aos_mr_get(int idx);
extern void     aos_mr_set(int idx, uint32_t val);

/* ── Storage op-code constants ───────────────────────────────────────────── */

/*
 * IPC label space for storage.v1.  Matches the values expected by the
 * agentOS VibeEngine conformance checker.
 */
#define STORAGE_OP_WRITE    0x30u   /* MR1=key_ptr MR2=key_len MR3=data_ptr MR4=data_len */
#define STORAGE_OP_READ     0x31u   /* MR1=key_ptr MR2=key_len → MR0=data_ptr MR1=data_len */
#define STORAGE_OP_DELETE   0x32u   /* MR1=key_ptr MR2=key_len */
#define STORAGE_OP_STAT     0x33u   /* MR1=key_ptr MR2=key_len → MR0=size MR1=exists */
#define STORAGE_OP_STAT_SVC 0x20u   /* → MR1=file_count MR2=total_bytes */
#define AOS_LABEL_HEALTH    0xFFFFu /* → return 0 (healthy) */

/* ── Table geometry ──────────────────────────────────────────────────────── */

#define MAX_FILES    64u
#define MAX_KEY_LEN  256u
#define MAX_VAL_LEN  4096u

/* ── In-process memory (WASM linear memory, not seL4 shared memory) ──────── */

typedef struct {
    uint8_t  key[MAX_KEY_LEN];
    uint32_t key_len;
    uint8_t  value[MAX_VAL_LEN];
    uint32_t value_len;
    uint32_t in_use;
} FileEntry;

/*
 * Static table.  Zero-initialised by the WASM runtime before service_init is
 * called (WASM linear memory is always zero-initialised per spec).
 */
static FileEntry g_table[MAX_FILES];
static uint32_t  g_file_count  = 0;
static uint32_t  g_total_bytes = 0;

/* ── Internal helpers ────────────────────────────────────────────────────── */

/*
 * Copy up to `n` bytes from a pointer encoded in a uint32_t MR value into
 * a local buffer.  In a real agentOS WASM service the pointer is an offset
 * into the WASM linear memory that both the runtime and the service share.
 * In this test implementation we treat the MR value directly as a host
 * pointer cast — safe because the WASM sandbox enforces bounds.
 */
static void mem_copy_in(uint8_t *dst, uint32_t src_mr_ptr, uint32_t len, uint32_t max)
{
    const uint8_t *src = (const uint8_t *)(uintptr_t)src_mr_ptr;
    uint32_t n = (len < max) ? len : max;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static int key_eq(const uint8_t *a, uint32_t a_len,
                  const uint8_t *b, uint32_t b_len)
{
    if (a_len != b_len) return 0;
    for (uint32_t i = 0; i < a_len; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/*
 * Find a table slot whose key matches.  Returns the index, or -1 if not found.
 */
static int find_entry(const uint8_t *key, uint32_t key_len)
{
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        if (g_table[i].in_use && key_eq(g_table[i].key, g_table[i].key_len, key, key_len)) {
            return (int)i;
        }
    }
    return -1;
}

/* Find a free slot, or -1 if the table is full. */
static int alloc_entry(void)
{
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        if (!g_table[i].in_use) return (int)i;
    }
    return -1;
}

/* ── Exported service entry points ───────────────────────────────────────── */

/*
 * service_init — called once when the WASM module is loaded into a swap slot.
 *
 * Zero-initialises the table (belt-and-suspenders; WASM linear memory is
 * already zero at module load, but explicit init is good practice).
 * Returns 0 on success.
 */
int service_init(void)
{
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        g_table[i].in_use     = 0;
        g_table[i].key_len    = 0;
        g_table[i].value_len  = 0;
    }
    g_file_count  = 0;
    g_total_bytes = 0;
    return 0;
}

/*
 * service_dispatch — IPC label dispatcher.
 *
 * label:     the IPC label from the caller (identifies the operation)
 * in_count:  number of valid input message registers (informational)
 * out_count: number of output message registers caller expects (informational)
 *
 * All operands are read from / written to the MRs directly via aos_mr_get and
 * aos_mr_set.  Returns 0 on success, -1 on unknown label, -2 on error.
 */
int service_dispatch(uint32_t label, uint32_t in_count, uint32_t out_count)
{
    (void)in_count;
    (void)out_count;

    uint8_t  key[MAX_KEY_LEN];
    uint32_t key_ptr, key_len;
    int      idx;

    switch (label) {

    /* ── STORAGE_OP_STAT_SVC (0x20) ────────────────────────────────────────
     * Returns aggregate service stats.
     * Out: MR1 = file_count, MR2 = total_bytes
     */
    case STORAGE_OP_STAT_SVC:
        aos_mr_set(0, 0);
        aos_mr_set(1, g_file_count);
        aos_mr_set(2, g_total_bytes);
        return 0;

    /* ── STORAGE_OP_WRITE (0x30) ────────────────────────────────────────────
     * Write (or overwrite) a key/value pair.
     * In:  MR1 = key_ptr, MR2 = key_len, MR3 = data_ptr, MR4 = data_len
     * Out: MR0 = 0 (ok) or -2 (table full / key too long / value too long)
     */
    case STORAGE_OP_WRITE: {
        key_ptr = aos_mr_get(1);
        key_len = aos_mr_get(2);
        uint32_t data_ptr = aos_mr_get(3);
        uint32_t data_len = aos_mr_get(4);

        if (key_len == 0 || key_len > MAX_KEY_LEN || data_len > MAX_VAL_LEN) {
            aos_mr_set(0, (uint32_t)-2);
            return -2;
        }

        mem_copy_in(key, key_ptr, key_len, MAX_KEY_LEN);

        idx = find_entry(key, key_len);
        if (idx < 0) {
            /* New entry */
            idx = alloc_entry();
            if (idx < 0) {
                aos_mr_set(0, (uint32_t)-2);
                return -2;
            }
            for (uint32_t i = 0; i < key_len; i++) {
                g_table[idx].key[i] = key[i];
            }
            g_table[idx].key_len = key_len;
            g_table[idx].in_use  = 1;
            g_file_count++;
        } else {
            /* Overwrite: subtract old value size from total */
            g_total_bytes -= g_table[idx].value_len;
        }

        mem_copy_in(g_table[idx].value, data_ptr, data_len, MAX_VAL_LEN);
        g_table[idx].value_len = data_len;
        g_total_bytes += data_len;

        aos_mr_set(0, 0);
        return 0;
    }

    /* ── STORAGE_OP_READ (0x31) ─────────────────────────────────────────────
     * Read the value for a key.
     * In:  MR1 = key_ptr, MR2 = key_len
     * Out: MR0 = data_ptr (pointer into WASM linear memory), MR1 = data_len
     *      MR0 = 0, MR1 = 0 if key not found
     */
    case STORAGE_OP_READ:
        key_ptr = aos_mr_get(1);
        key_len = aos_mr_get(2);

        if (key_len == 0 || key_len > MAX_KEY_LEN) {
            aos_mr_set(0, 0);
            aos_mr_set(1, 0);
            return -2;
        }

        mem_copy_in(key, key_ptr, key_len, MAX_KEY_LEN);
        idx = find_entry(key, key_len);

        if (idx < 0) {
            aos_mr_set(0, 0);
            aos_mr_set(1, 0);
        } else {
            /*
             * Return a pointer to the value buffer inside WASM linear memory.
             * The caller is responsible for reading it before the next dispatch.
             */
            aos_mr_set(0, (uint32_t)(uintptr_t)g_table[idx].value);
            aos_mr_set(1, g_table[idx].value_len);
        }
        return 0;

    /* ── STORAGE_OP_DELETE (0x32) ───────────────────────────────────────────
     * Delete a key/value pair.
     * In:  MR1 = key_ptr, MR2 = key_len
     * Out: MR0 = 0 (ok), -1 (not found)
     */
    case STORAGE_OP_DELETE:
        key_ptr = aos_mr_get(1);
        key_len = aos_mr_get(2);

        if (key_len == 0 || key_len > MAX_KEY_LEN) {
            aos_mr_set(0, (uint32_t)-1);
            return -1;
        }

        mem_copy_in(key, key_ptr, key_len, MAX_KEY_LEN);
        idx = find_entry(key, key_len);

        if (idx < 0) {
            aos_mr_set(0, (uint32_t)-1);
            return -1;
        }

        g_total_bytes -= g_table[idx].value_len;
        g_table[idx].in_use    = 0;
        g_table[idx].key_len   = 0;
        g_table[idx].value_len = 0;
        g_file_count--;

        aos_mr_set(0, 0);
        return 0;

    /* ── STORAGE_OP_STAT (0x33) ─────────────────────────────────────────────
     * Stat a single key.
     * In:  MR1 = key_ptr, MR2 = key_len
     * Out: MR0 = value_size, MR1 = 1 (exists) or 0 (not found)
     */
    case STORAGE_OP_STAT:
        key_ptr = aos_mr_get(1);
        key_len = aos_mr_get(2);

        if (key_len == 0 || key_len > MAX_KEY_LEN) {
            aos_mr_set(0, 0);
            aos_mr_set(1, 0);
            return -2;
        }

        mem_copy_in(key, key_ptr, key_len, MAX_KEY_LEN);
        idx = find_entry(key, key_len);

        if (idx < 0) {
            aos_mr_set(0, 0);
            aos_mr_set(1, 0);
        } else {
            aos_mr_set(0, g_table[idx].value_len);
            aos_mr_set(1, 1);
        }
        return 0;

    /* ── AOS_LABEL_HEALTH (0xFFFF) ──────────────────────────────────────────
     * Health probe — always returns 0 (healthy).
     */
    case AOS_LABEL_HEALTH:
        aos_mr_set(0, 0);
        return 0;

    default:
        return -1;
    }
}

/*
 * service_health — called by the VibeEngine sandbox-test harness.
 *
 * Returns 0 to indicate the service is healthy.  This is separate from the
 * in-band AOS_LABEL_HEALTH dispatch path so the runtime can probe without
 * going through the full dispatch switch.
 */
int service_health(void)
{
    return 0;
}
