# agentOS Service Code Generation Prompt

This document is the canonical prompt template injected by the agentOS bridge
when requesting code generation for a hot-swappable service. The LLM must
produce a single C99 source file that conforms to the agentOS Service ABI.

---

## agentOS Service Model

agentOS is a seL4-based operating system for AI agents. Services run as
Protection Domains (PDs) under the Microkit framework. The hot-swap mechanism
lets running services be replaced at runtime without rebooting the system.

### How Hot-Swap Works

1. An agent writes a new WASM binary to the shared staging region.
2. The VibeEngine validates the binary (magic header, size, capability check).
3. The VibeEngine notifies the controller, which calls `vibe_swap_begin()`.
4. The controller loads the WASM into a pre-allocated swap slot PD via shared
   memory.
5. The vibe_swap layer runs conformance tests (health check, service-specific
   functional tests) against the new slot.
6. If all tests pass, the controller's routing table is atomically updated so
   that future IPC to this service reaches the new slot.
7. The old slot is held warm as a rollback target.

### Swap Slot PD

A swap slot PD is a generic "service worker" that reads its WASM binary from a
shared memory region (mapped read-only at `0x2000000`) and interprets it via
the embedded wasm3 runtime. The slot exports the three mandatory entry points
via the WASM module's export table.

---

## ABI Header

Every generated service source file must be compatible with the following
header. The header is provided to the compiler via `-include agentos_service_abi.h`.

```c
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
```

---

## storage.v1 Operation Table

The following table defines all IPC operations for a storage service (the
`memfs` replacement). Each entry shows the operation label, input message
registers (MRs), and the expected output MRs on a successful reply (MR0=0).

| Label | Name              | Input MRs                                           | Output MRs (success)                   |
|-------|-------------------|-----------------------------------------------------|----------------------------------------|
| 0x30  | STORAGE_OP_WRITE  | MR1=path_ptr, MR2=path_len, MR3=data_ptr, MR4=data_len | MR0=0 (ok)                        |
| 0x31  | STORAGE_OP_READ   | MR1=path_ptr, MR2=path_len                          | MR0=data_ptr, MR1=data_len            |
| 0x32  | STORAGE_OP_DELETE | MR1=path_ptr, MR2=path_len                          | MR0=0 (ok)                            |
| 0x33  | STORAGE_OP_STAT   | MR1=path_ptr, MR2=path_len                          | MR0=size, MR1=flags                   |
| 0x34  | STORAGE_OP_LIST   | MR1=prefix_ptr, MR2=prefix_len                      | MR0=entries_ptr, MR1=entry_count      |
| 0x20  | STORAGE_OP_STAT_SVC | (none)                                            | MR1=file_count, MR2=total_bytes       |
| 0xFFFF | AOS_LABEL_HEALTH | (none)                                             | MR0=0 (healthy)                        |

**Error convention**: on error, MR0 is a negative errno cast to uint32_t
(e.g., `(uint32_t)-1` for EPERM, `(uint32_t)-2` for ENOENT).

**Pointer convention**: `path_ptr` and `data_ptr` are offsets into the WASM
module's linear memory. The runtime resolves them before calling
`service_dispatch`. The service may read/write these memory regions freely.

---

## Constraints

You MUST follow these constraints or the conformance tests will reject the service:

1. **C99 only**: The source file must compile with `-std=c99`. No C11, no C++,
   no GCC/Clang extensions except `__attribute__((export_name(...)))` on the
   three mandatory functions.

2. **No dynamic allocation in the dispatch path**: `service_dispatch` and
   `service_health` must not call `malloc`, `calloc`, `realloc`, or `free`.
   All working storage must be statically allocated (file-scope arrays or
   fixed-size stack buffers). Rationale: bounded latency is required; the
   swap slot runs at a real-time priority.

3. **service_health must return in under 1ms**: The health probe is called
   periodically by the vibe_swap manager. If it does not return before the
   timeout, the slot is considered unhealthy and the service is rolled back.
   Do not perform I/O, loops over large data structures, or IPC in
   `service_health`.

4. **No global mutable state except the service's own data structures**: You
   may declare file-scope variables to hold the service's state (e.g., a
   key/value table for a storage service). You must not use POSIX thread-local
   storage, signal handlers, or any other mechanism that escapes the WASM
   linear memory model.

5. **All three mandatory functions must be exported**: Add
   `__attribute__((export_name("service_init")))`,
   `__attribute__((export_name("service_dispatch")))`, and
   `__attribute__((export_name("service_health")))` to the function
   definitions. The wasm3 runtime locates these by name.

6. **No libc I/O**: Do not use `printf`, `fprintf`, `puts`, `fopen`, or any
   other stdio function. Use `aos_log_str` for diagnostic output.

---

## Minimal Conformant Example: storage.v1 Passthrough to Original MemFS

The following example shows a minimal storage service that passes all
conformance tests. It implements a flat in-memory key/value store backed by
a static array, and responds to the full `storage.v1` operation set.

This is a useful starting point when replacing `memfs` because the conformance
tests for `SVC_MEMFS` (service_id=1) exercise STORAGE_OP_WRITE, STORAGE_OP_READ,
and AOS_LABEL_HEALTH in sequence.

```c
/*
 * storage_passthrough.c — minimal agentOS storage.v1 service
 *
 * Implements a flat key/value store using a fixed-size static table.
 * Conforms to the agentOS Service ABI.
 */

/* agentos_service_abi.h is injected by the build system via -include */

/* ── Storage table ──────────────────────────────────────────────────────── */
#define MAX_ENTRIES   64
#define MAX_KEY_LEN   128
#define MAX_VAL_LEN   4096

typedef struct {
    char     key[MAX_KEY_LEN];
    uint8_t  val[MAX_VAL_LEN];
    uint32_t key_len;
    uint32_t val_len;
    int      used;
} kv_entry_t;

static kv_entry_t g_table[MAX_ENTRIES];
static uint32_t   g_entry_count = 0;

/* ── Internal helpers ───────────────────────────────────────────────────── */

static int kv_find(const char *key, uint32_t key_len) {
    for (uint32_t i = 0; i < MAX_ENTRIES; i++) {
        if (g_table[i].used && g_table[i].key_len == key_len) {
            int match = 1;
            for (uint32_t j = 0; j < key_len; j++) {
                if (g_table[i].key[j] != key[j]) { match = 0; break; }
            }
            if (match) return (int)i;
        }
    }
    return -1;
}

static int kv_alloc(void) {
    for (uint32_t i = 0; i < MAX_ENTRIES; i++) {
        if (!g_table[i].used) return (int)i;
    }
    return -1;
}

static void kv_copy(char *dst, const char *src, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
}

/* ── ABI entry points ───────────────────────────────────────────────────── */

__attribute__((export_name("service_init")))
int service_init(void) {
    for (uint32_t i = 0; i < MAX_ENTRIES; i++) {
        g_table[i].used    = 0;
        g_table[i].key_len = 0;
        g_table[i].val_len = 0;
    }
    g_entry_count = 0;
    static const char msg[] = "[storage_passthrough] init ok\n";
    aos_log_str(msg, sizeof(msg) - 1);
    return 0;
}

__attribute__((export_name("service_dispatch")))
int service_dispatch(uint32_t label, uint32_t in_count, uint32_t out_count) {
    (void)in_count;
    (void)out_count;

    switch (label) {

    case STORAGE_OP_WRITE: {
        /* MR1=path_ptr MR2=path_len MR3=data_ptr MR4=data_len */
        const char *path     = (const char *)(uintptr_t)aos_mr_get(1);
        uint32_t    path_len = aos_mr_get(2);
        const char *data     = (const char *)(uintptr_t)aos_mr_get(3);
        uint32_t    data_len = aos_mr_get(4);

        if (path_len == 0 || path_len >= MAX_KEY_LEN) {
            aos_mr_set(0, (uint32_t)-1);
            return -1;
        }
        if (data_len > MAX_VAL_LEN) {
            aos_mr_set(0, (uint32_t)-1);
            return -1;
        }

        int idx = kv_find(path, path_len);
        if (idx < 0) {
            idx = kv_alloc();
            if (idx < 0) { aos_mr_set(0, (uint32_t)-1); return -1; }
            g_entry_count++;
        }

        kv_copy(g_table[idx].key, path, path_len);
        g_table[idx].key_len = path_len;
        for (uint32_t i = 0; i < data_len; i++)
            g_table[idx].val[i] = (uint8_t)data[i];
        g_table[idx].val_len = data_len;
        g_table[idx].used    = 1;

        aos_mr_set(0, 0);
        return 0;
    }

    case STORAGE_OP_READ: {
        /* MR1=path_ptr MR2=path_len → MR0=data_ptr MR1=data_len */
        const char *path     = (const char *)(uintptr_t)aos_mr_get(1);
        uint32_t    path_len = aos_mr_get(2);

        int idx = kv_find(path, path_len);
        if (idx < 0) {
            aos_mr_set(0, 0);
            aos_mr_set(1, 0);
            return -1;
        }

        aos_mr_set(0, (uint32_t)(uintptr_t)g_table[idx].val);
        aos_mr_set(1, g_table[idx].val_len);
        return 0;
    }

    case STORAGE_OP_DELETE: {
        const char *path     = (const char *)(uintptr_t)aos_mr_get(1);
        uint32_t    path_len = aos_mr_get(2);

        int idx = kv_find(path, path_len);
        if (idx < 0) { aos_mr_set(0, (uint32_t)-2); return -1; }

        g_table[idx].used    = 0;
        g_table[idx].key_len = 0;
        g_table[idx].val_len = 0;
        g_entry_count--;

        aos_mr_set(0, 0);
        return 0;
    }

    case STORAGE_OP_STAT: {
        const char *path     = (const char *)(uintptr_t)aos_mr_get(1);
        uint32_t    path_len = aos_mr_get(2);

        int idx = kv_find(path, path_len);
        if (idx < 0) { aos_mr_set(0, (uint32_t)-2); return -1; }

        aos_mr_set(0, g_table[idx].val_len);  /* size */
        aos_mr_set(1, 0);                      /* flags: regular entry */
        return 0;
    }

    case STORAGE_OP_LIST: {
        /* Returns number of entries whose key starts with prefix.
         * Writes packed null-terminated keys to val buffer of entry 0
         * (a scratch area — safe because we own all linear memory). */
        const char *prefix     = (const char *)(uintptr_t)aos_mr_get(1);
        uint32_t    prefix_len = aos_mr_get(2);

        /* Use a static scratch buffer for the list output */
        static uint8_t list_buf[MAX_ENTRIES * MAX_KEY_LEN];
        uint32_t buf_pos = 0;
        uint32_t count   = 0;

        for (uint32_t i = 0; i < MAX_ENTRIES; i++) {
            if (!g_table[i].used) continue;
            if (g_table[i].key_len < prefix_len) continue;

            int match = 1;
            for (uint32_t j = 0; j < prefix_len; j++) {
                if (g_table[i].key[j] != prefix[j]) { match = 0; break; }
            }
            if (!match) continue;

            /* Append key + NUL to list buffer */
            uint32_t kl = g_table[i].key_len;
            if (buf_pos + kl + 1 > sizeof(list_buf)) break;
            for (uint32_t j = 0; j < kl; j++) list_buf[buf_pos++] = (uint8_t)g_table[i].key[j];
            list_buf[buf_pos++] = 0;
            count++;
        }

        aos_mr_set(0, (uint32_t)(uintptr_t)list_buf);
        aos_mr_set(1, count);
        return 0;
    }

    case STORAGE_OP_STAT_SVC: {
        /* MR0=op → MR1=file_count MR2=total_bytes */
        uint32_t total_bytes = 0;
        for (uint32_t i = 0; i < MAX_ENTRIES; i++) {
            if (g_table[i].used) total_bytes += g_table[i].val_len;
        }
        aos_mr_set(0, 0);
        aos_mr_set(1, g_entry_count);
        aos_mr_set(2, total_bytes);
        return 0;
    }

    case AOS_LABEL_HEALTH:
        /* Health probe — must return 0 immediately */
        aos_mr_set(0, 0);
        return 0;

    default:
        aos_mr_set(0, (uint32_t)-1);
        return -1;
    }
}

__attribute__((export_name("service_health")))
int service_health(void) {
    /* Sanity check: count used entries and verify count matches g_entry_count */
    uint32_t actual = 0;
    for (uint32_t i = 0; i < MAX_ENTRIES; i++) {
        if (g_table[i].used) actual++;
    }
    return (actual == g_entry_count) ? 0 : -1;
}
```

---

## Compile Command

```sh
clang --target=wasm32-wasi \
      -O2 \
      -std=c99 \
      -nostdlib \
      -Wl,--no-entry \
      -Wl,--export-all \
      -Wl,--allow-undefined \
      -include /path/to/agentos_service_abi.h \
      -o storage_passthrough.wasm \
      storage_passthrough.c
```

The resulting `storage_passthrough.wasm` can be written to the VibeEngine's
staging region and proposed via `OP_VIBE_PROPOSE` (opcode 0x40). The service
ID for `memfs` is `AOS_SVC_MEMFS` (1).

---

## Summary Checklist for Code Generation

Before returning generated code, verify:

- [ ] Implements `service_init`, `service_dispatch`, and `service_health`
- [ ] All three functions have `__attribute__((export_name(...)))`
- [ ] No `malloc`/`free` in `service_dispatch` or `service_health`
- [ ] `service_health` has no loops over unbounded data, no IPC
- [ ] All storage is statically allocated (file-scope or fixed stack)
- [ ] Uses `aos_mr_get`/`aos_mr_set` for all IPC register access
- [ ] Uses `aos_log_str` instead of `printf`/`puts`
- [ ] Handles `AOS_LABEL_HEALTH` (0xFFFF) in `service_dispatch`
- [ ] Returns 0 on success, negative cast to uint32_t on error in MR0
- [ ] Compiles cleanly with `-std=c99 -Wall -Wextra` (no warnings)
