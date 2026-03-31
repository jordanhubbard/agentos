/*
 * agentOS WASM3 Host — Implementation
 *
 * Bridges wasm3 interpreter with agentOS swap slot protection domains.
 * This is where the vibe-coding magic happens: agent-generated WASM
 * modules get loaded and executed inside capability-isolated PDs.
 *
 * Architecture:
 *   - wasm3 runs as an interpreter inside the swap slot PD
 *   - Host imports provide controlled access to OS services
 *   - The WASM module exports init/handle_ppc/health_check
 *   - All execution is within the PD's capability boundary
 *
 * Memory layout within the code region (4MB at 0x2000000):
 *   [0x000000] swap_slot_header_t (128 bytes)
 *   [0x000080] WASM binary (up to ~1MB)
 *   [0x200000] PPC result buffer (40 bytes)
 *   [0x200030] Data region for aos_mem_read/write (64KB)
 *   [0x210030] (remaining: available for WASM linear memory growth)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <microkit.h>
#include "wasm3_host.h"

/* Include wasm3 — these reference the vendored source */
#include "wasm3.h"
#include "m3_env.h"

/* Code region layout constants */
#define CODE_REGION_BASE    0x2000000UL
#define PPC_RESULT_OFFSET   0x200000UL  /* Offset within WASM linear memory */
#define DATA_REGION_OFFSET  0x200030UL
#define DATA_REGION_SIZE    0x10000UL   /* 64KB data region */

/* WASM stack size for the interpreter */
#define WASM_STACK_SIZE     (64 * 1024)

/* Host state — lives in BSS (one per swap slot PD) */
struct wasm3_host {
    IM3Environment  env;
    IM3Runtime      runtime;
    IM3Module       module;
    IM3Function     fn_init;
    IM3Function     fn_handle_ppc;
    IM3Function     fn_health_check;
    IM3Function     fn_heap_stats;   /* optional: __agentos_heap_stats(i32*, i32*) */
    bool            initialized;
};

/* Single static instance (one WASM module per swap slot PD) */
static struct wasm3_host host_state;

/* ---- Host Import Implementations ---- */

/*
 * aos_log(level: i32, msg_ptr: i32, msg_len: i32)
 *
 * Log a message from WASM to the Microkit debug console.
 * msg_ptr/msg_len reference WASM linear memory.
 */
static m3ApiRawFunction(host_aos_log)
{
    m3ApiGetArg     (int32_t, level)
    m3ApiGetArg     (int32_t, msg_ptr)
    m3ApiGetArg     (int32_t, msg_len)

    (void)level;  /* TODO: filter by log level */

    /* Get pointer into WASM linear memory */
    uint32_t mem_size = 0;
    uint8_t *mem = (uint8_t *)m3_GetMemory(runtime, &mem_size, 0);

    if (mem && msg_ptr >= 0 && (uint32_t)(msg_ptr + msg_len) <= mem_size) {
        /* Print character by character via dbg_puts (no buffer overflow risk) */
        char buf[128];
        int copy_len = msg_len;
        if (copy_len > 127) copy_len = 127;

        for (int i = 0; i < copy_len; i++) {
            buf[i] = (char)mem[msg_ptr + i];
        }
        buf[copy_len] = '\0';

        microkit_dbg_puts("[wasm] ");
        microkit_dbg_puts(buf);
        microkit_dbg_puts("\n");
    }

    m3ApiSuccess();
}

/*
 * aos_time_us() -> i64
 *
 * Return current time in microseconds.
 * On bare metal with no timer configured, returns 0.
 * TODO: Wire to seL4 timer when MCS scheduling is available.
 */
static m3ApiRawFunction(host_aos_time_us)
{
    m3ApiReturnType (int64_t)
    m3ApiReturn     (0)
}

/*
 * aos_mem_read(offset: i32, buf_ptr: i32, len: i32) -> i32
 *
 * Read from the data region into WASM linear memory.
 * Returns number of bytes read, or -1 on error.
 */
static m3ApiRawFunction(host_aos_mem_read)
{
    m3ApiReturnType (int32_t)
    m3ApiGetArg     (int32_t, offset)
    m3ApiGetArg     (int32_t, buf_ptr)
    m3ApiGetArg     (int32_t, len)

    if (offset < 0 || len < 0 || (uint32_t)offset + (uint32_t)len > DATA_REGION_SIZE) {
        m3ApiReturn(-1)
    }

    uint32_t mem_size = 0;
    uint8_t *mem = (uint8_t *)m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || (uint32_t)(buf_ptr + len) > mem_size) {
        m3ApiReturn(-1)
    }

    /* Read from data region (relative to code region base) */
    volatile uint8_t *data = (volatile uint8_t *)(CODE_REGION_BASE + DATA_REGION_OFFSET);
    for (int32_t i = 0; i < len; i++) {
        mem[buf_ptr + i] = data[offset + i];
    }

    m3ApiReturn(len)
}

/*
 * aos_mem_write(offset: i32, buf_ptr: i32, len: i32) -> i32
 *
 * Write from WASM linear memory to the data region.
 * Returns number of bytes written, or -1 on error.
 */
static m3ApiRawFunction(host_aos_mem_write)
{
    m3ApiReturnType (int32_t)
    m3ApiGetArg     (int32_t, offset)
    m3ApiGetArg     (int32_t, buf_ptr)
    m3ApiGetArg     (int32_t, len)

    if (offset < 0 || len < 0 || (uint32_t)offset + (uint32_t)len > DATA_REGION_SIZE) {
        m3ApiReturn(-1)
    }

    uint32_t mem_size = 0;
    uint8_t *mem = (uint8_t *)m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || (uint32_t)(buf_ptr + len) > mem_size) {
        m3ApiReturn(-1)
    }

    volatile uint8_t *data = (volatile uint8_t *)(CODE_REGION_BASE + DATA_REGION_OFFSET);
    for (int32_t i = 0; i < len; i++) {
        data[offset + i] = mem[buf_ptr + i];
    }

    m3ApiReturn(len)
}

/* ---- Host API ---- */

/*
 * Link all host imports into the module.
 */
static M3Result link_host_imports(IM3Module module) {
    M3Result result;
    const char *env = "aos";  /* WASM import module name */

    result = m3_LinkRawFunction(module, env, "aos_log",
                                 "v(iii)", &host_aos_log);
    if (result && result != m3Err_functionLookupFailed) return result;

    result = m3_LinkRawFunction(module, env, "aos_time_us",
                                 "I()", &host_aos_time_us);
    if (result && result != m3Err_functionLookupFailed) return result;

    result = m3_LinkRawFunction(module, env, "aos_mem_read",
                                 "i(iii)", &host_aos_mem_read);
    if (result && result != m3Err_functionLookupFailed) return result;

    result = m3_LinkRawFunction(module, env, "aos_mem_write",
                                 "i(iii)", &host_aos_mem_write);
    if (result && result != m3Err_functionLookupFailed) return result;

    return m3Err_none;
}

wasm3_host_t *wasm3_host_init(const uint8_t *wasm_bytes, uint32_t wasm_size) {
    wasm3_host_t *host = &host_state;
    M3Result result;

    microkit_dbg_puts("[wasm3_host] Initializing WASM runtime\n");

    /* Reset state */
    host->env = NULL;
    host->runtime = NULL;
    host->module = NULL;
    host->fn_init = NULL;
    host->fn_handle_ppc = NULL;
    host->fn_health_check = NULL;
    host->initialized = false;

    /* Create environment */
    host->env = m3_NewEnvironment();
    if (!host->env) {
        microkit_dbg_puts("[wasm3_host] ERROR: failed to create environment\n");
        return NULL;
    }

    /* Create runtime with stack */
    host->runtime = m3_NewRuntime(host->env, WASM_STACK_SIZE, NULL);
    if (!host->runtime) {
        microkit_dbg_puts("[wasm3_host] ERROR: failed to create runtime\n");
        return NULL;
    }

    /* Parse the WASM module */
    microkit_dbg_puts("[wasm3_host] Parsing WASM module (");
    /* Print size — simple decimal */
    char sizebuf[12];
    int si = 0;
    uint32_t sz = wasm_size;
    char tmp[12];
    int ti = 0;
    if (sz == 0) { tmp[ti++] = '0'; }
    else { while (sz > 0) { tmp[ti++] = '0' + (sz % 10); sz /= 10; } }
    for (int j = ti - 1; j >= 0; j--) sizebuf[si++] = tmp[j];
    sizebuf[si] = '\0';
    microkit_dbg_puts(sizebuf);
    microkit_dbg_puts(" bytes)\n");

    result = m3_ParseModule(host->env, &host->module, wasm_bytes, wasm_size);
    if (result) {
        microkit_dbg_puts("[wasm3_host] ERROR: parse failed: ");
        microkit_dbg_puts(result);
        microkit_dbg_puts("\n");
        return NULL;
    }

    /* Load module into runtime */
    result = m3_LoadModule(host->runtime, host->module);
    if (result) {
        microkit_dbg_puts("[wasm3_host] ERROR: load failed: ");
        microkit_dbg_puts(result);
        microkit_dbg_puts("\n");
        return NULL;
    }

    /* Link host imports */
    result = link_host_imports(host->module);
    if (result) {
        microkit_dbg_puts("[wasm3_host] ERROR: link failed: ");
        microkit_dbg_puts(result);
        microkit_dbg_puts("\n");
        return NULL;
    }

    /* Compile all functions */
    result = m3_CompileModule(host->module);
    if (result) {
        microkit_dbg_puts("[wasm3_host] WARNING: compile incomplete: ");
        microkit_dbg_puts(result);
        microkit_dbg_puts("\n");
        /* Non-fatal — some functions may still work */
    }

    /* Find exported functions */
    result = m3_FindFunction(&host->fn_init, host->runtime, "init");
    if (result) {
        microkit_dbg_puts("[wasm3_host] WARNING: no init() export\n");
        host->fn_init = NULL;
    }

    result = m3_FindFunction(&host->fn_handle_ppc, host->runtime, "handle_ppc");
    if (result) {
        microkit_dbg_puts("[wasm3_host] WARNING: no handle_ppc() export\n");
        host->fn_handle_ppc = NULL;
    }

    result = m3_FindFunction(&host->fn_health_check, host->runtime, "health_check");
    if (result) {
        microkit_dbg_puts("[wasm3_host] WARNING: no health_check() export\n");
        host->fn_health_check = NULL;
    }

    /* Optional: __agentos_heap_stats(live_allocs_ptr: i32, live_bytes_ptr: i32)
     * Exported by nanolang WASM modules compiled with refcount_gc.h.
     * If not present, mem_profiler falls back to wasm3 linear-memory scan. */
    result = m3_FindFunction(&host->fn_heap_stats, host->runtime, "__agentos_heap_stats");
    if (result) {
        host->fn_heap_stats = NULL;  /* Optional — no warning */
    }

    host->initialized = true;
    microkit_dbg_puts("[wasm3_host] WASM module loaded successfully\n");
    return host;
}

bool wasm3_host_call_init(wasm3_host_t *host) {
    if (!host || !host->initialized || !host->fn_init) return false;

    microkit_dbg_puts("[wasm3_host] Calling WASM init()\n");

    M3Result result = m3_CallV(host->fn_init);
    if (result) {
        microkit_dbg_puts("[wasm3_host] ERROR: init() failed: ");
        microkit_dbg_puts(result);
        microkit_dbg_puts("\n");
        return false;
    }

    microkit_dbg_puts("[wasm3_host] init() completed\n");
    return true;
}

bool wasm3_host_call_health_check(wasm3_host_t *host) {
    if (!host || !host->initialized || !host->fn_health_check) return false;

    M3Result result = m3_CallV(host->fn_health_check);
    if (result) {
        microkit_dbg_puts("[wasm3_host] ERROR: health_check() failed\n");
        return false;
    }

    /* Get return value */
    int32_t retval = 0;
    result = m3_GetResultsV(host->fn_health_check, &retval);
    if (result) return false;

    return (retval != 0);
}

bool wasm3_host_call_ppc(wasm3_host_t *host,
                          uint64_t label, uint64_t mr0, uint64_t mr1,
                          uint64_t mr2, uint64_t mr3,
                          wasm_ppc_result_t *out) {
    if (!host || !host->initialized || !host->fn_handle_ppc || !out) return false;

    /*
     * Strategy for returning multiple values from WASM:
     *
     * Since WASM multi-value returns are complex and not all wasm3 versions
     * support them well, we use a shared memory approach:
     *
     * 1. Call handle_ppc(label, mr0, mr1, mr2, mr3) — returns i32 status
     * 2. The WASM module writes results to linear memory at PPC_RESULT_OFFSET:
     *    [+0]  label (i64)
     *    [+8]  mr0   (i64)
     *    [+16] mr1   (i64)
     *    [+24] mr2   (i64)
     *    [+32] mr3   (i64)
     * 3. We read the results back from WASM linear memory
     */

    M3Result result = m3_CallV(host->fn_handle_ppc,
                                (int64_t)label, (int64_t)mr0,
                                (int64_t)mr1, (int64_t)mr2, (int64_t)mr3);
    if (result) {
        microkit_dbg_puts("[wasm3_host] ERROR: handle_ppc() failed: ");
        microkit_dbg_puts(result);
        microkit_dbg_puts("\n");
        return false;
    }

    /* Read results from WASM linear memory */
    uint32_t mem_size = 0;
    uint8_t *mem = (uint8_t *)m3_GetMemory(host->runtime, &mem_size, 0);
    if (!mem || mem_size < PPC_RESULT_OFFSET + 40) {
        microkit_dbg_puts("[wasm3_host] ERROR: WASM memory too small for PPC results\n");
        return false;
    }

    uint64_t *results = (uint64_t *)(mem + PPC_RESULT_OFFSET);
    out->label = results[0];
    out->mr0   = results[1];
    out->mr1   = results[2];
    out->mr2   = results[3];
    out->mr3   = results[4];

    return true;
}

/*
 * wasm3_host_get_heap_stats() — query live alloc count and live bytes from the
 * WASM module's refcount_gc heap via the __agentos_heap_stats export.
 *
 * This is called by mem_profiler via the worker PPC hook to populate leak
 * detection counters without scanning linear memory directly.
 *
 * Returns false if the module does not export __agentos_heap_stats (old modules
 * compiled without refcount_gc.h).  In that case mem_profiler uses a fallback.
 */
bool wasm3_host_get_heap_stats(wasm3_host_t *host,
                                uint32_t     *live_allocs_out,
                                uint32_t     *live_bytes_out) {
    if (!host || !host->initialized || !host->fn_heap_stats) return false;

    /* The WASM function signature: (i32 live_allocs_ptr, i32 live_bytes_ptr) -> void
     * We pass linear-memory addresses for two uint32_t slots in the WASM heap.
     * Since we're the host, we use a scratch region in the PPC result buffer. */

    /* Use the last 8 bytes of the PPC result buffer as scratch (safe: not in use
     * during heap stats query which happens outside PPC handling). */
    uintptr_t base = CODE_REGION_BASE + PPC_RESULT_OFFSET;
    uint32_t alloc_wasm_ptr = (uint32_t)(base + 0);   /* wasm linear-mem offset */
    uint32_t bytes_wasm_ptr = (uint32_t)(base + 4);

    /* Zero the scratch area */
    *((volatile uint32_t *)(uintptr_t)alloc_wasm_ptr) = 0;
    *((volatile uint32_t *)(uintptr_t)bytes_wasm_ptr) = 0;

    M3Result result = m3_CallV(host->fn_heap_stats,
                               (uint32_t)alloc_wasm_ptr,
                               (uint32_t)bytes_wasm_ptr);
    if (result) return false;

    if (live_allocs_out) *live_allocs_out = *((uint32_t *)(uintptr_t)alloc_wasm_ptr);
    if (live_bytes_out)  *live_bytes_out  = *((uint32_t *)(uintptr_t)bytes_wasm_ptr);
    return true;
}

void wasm3_host_destroy(wasm3_host_t *host) {
    if (!host) return;

    microkit_dbg_puts("[wasm3_host] Destroying WASM runtime\n");

    /* With fixed heap, these free calls are mostly no-ops,
     * but they reset internal state properly. */
    if (host->runtime) {
        m3_FreeRuntime(host->runtime);
    }
    if (host->env) {
        m3_FreeEnvironment(host->env);
    }

    host->env = NULL;
    host->runtime = NULL;
    host->module = NULL;
    host->fn_init = NULL;
    host->fn_handle_ppc = NULL;
    host->fn_health_check = NULL;
    host->initialized = false;
}
