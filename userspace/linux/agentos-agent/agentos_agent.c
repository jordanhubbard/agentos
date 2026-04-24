/*
 * agentos_agent — agentOS Linux guest agent
 *
 * Runs inside the Linux guest after kernel boot, spawned by ipc_bridge_daemon
 * in response to an IPC_OP_SPAWN command from the seL4 root task.
 *
 * Pipeline:
 *   1. Map /dev/mem at IPC_SHMEM_BASE to access the IPC shared-memory region.
 *   2. Wait for IPC_CMD_MAGIC to appear in the command ring header (seL4 side
 *      has initialised the bridge). Timeout: BRIDGE_WAIT_TIMEOUT_MS.
 *   3. Write the WASM service module into the staging area (at
 *      AGENT_STAGING_OFFSET within the mapped region) — the built-in
 *      g_service_wasm[] fixture or an externally supplied binary.
 *   4. Write a response into the Linux→seL4 response ring:
 *        op=IPC_OP_SPAWN, status=0, payload="wasm_ready:<wasm_size_hex>"
 *   5. Exit 0.  The seL4 debug_bridge PD drains the response ring and
 *      triggers VSWAP_OP_PROPOSE on the vibe-engine with the staging bytes.
 *
 * The staging region physical address (IPC_SHMEM_BASE + AGENT_STAGING_OFFSET)
 * must match the MR the seL4 root task maps for the staging shmem window used
 * by the vibe-engine.  Both sides agree on AGENT_STAGING_OFFSET by convention.
 *
 * Build:
 *   make -C userspace/linux/agentos-agent
 * or:
 *   aarch64-linux-gnu-gcc -O2 -o agentos-agent agentos_agent.c
 *   x86_64-linux-gnu-gcc  -O2 -o agentos-agent agentos_agent.c
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ── IPC bridge constants (mirrors ipc_bridge.h) ───────────────────────────
 * Kept inline so this file compiles standalone inside the Linux guest without
 * access to the seL4 kernel source tree.
 */

#define IPC_SHMEM_BASE          0x4000000UL
#define IPC_SHMEM_SIZE          0x20000UL     /* 128 KB total shmem window */

#define IPC_CMD_RING_OFFSET     0x1000UL
#define IPC_RESP_RING_OFFSET    0x2000UL

#define IPC_CMD_MAGIC           0x49504343UL  /* "IPCC" */
#define IPC_RESP_MAGIC          0x49504352UL  /* "IPCR" */

#define IPC_OP_EXEC             0x01
#define IPC_OP_SPAWN            0x05

#define IPC_RING_DEPTH          64
#define IPC_PAYLOAD_LEN         128

/* ── Staging region ─────────────────────────────────────────────────────────
 * The agent writes the WASM module here.  The seL4 debug_bridge PD reads from
 * the same physical frame to stage the PROPOSE call to the vibe-engine.
 * Offset is from IPC_SHMEM_BASE; must match the MR in the system descriptor.
 */
#define AGENT_STAGING_OFFSET    0x10000UL     /* 64 KB into the shmem window */
#define AGENT_STAGING_SIZE      0x10000UL     /* 64 KB staging area          */

/* ── Wait parameters ────────────────────────────────────────────────────── */
#define BRIDGE_WAIT_TIMEOUT_MS  10000         /* 10 s */
#define BRIDGE_POLL_INTERVAL_MS 50

/* ── IPC ring structs (must match ipc_bridge.h) ─────────────────────────── */

typedef struct {
    uint32_t seq;
    uint32_t op;
    uint32_t vm_slot;
    uint32_t payload_len;
    uint8_t  payload[IPC_PAYLOAD_LEN];
} ipc_cmd_t;

typedef struct {
    uint32_t seq;
    uint32_t status;
    uint32_t payload_len;
    uint32_t _pad;
    uint8_t  payload[IPC_PAYLOAD_LEN];
} ipc_resp_t;

typedef struct {
    uint32_t  magic;
    uint32_t  head;
    uint32_t  tail;
    uint32_t  _pad;
    ipc_cmd_t cmds[IPC_RING_DEPTH];
} ipc_cmd_ring_t;

typedef struct {
    uint32_t   magic;
    uint32_t   head;
    uint32_t   tail;
    uint32_t   _pad;
    ipc_resp_t resps[IPC_RING_DEPTH];
} ipc_resp_ring_t;

/* ── Embedded minimal WASM service module ───────────────────────────────────
 * Hand-crafted binary satisfying the vibe-engine validator requirements:
 *   - WASM magic + version 1
 *   - Memory section (1 page)
 *   - Exports: init, handle_ppc, health_check, notified (functions), memory
 *   - Custom section "agentos.capabilities"
 *
 * See tests/wasm_fixtures/minimal_service.h for the annotated layout.
 * An external WASM binary can be passed via argv[1] to override this fixture.
 */
static const uint8_t g_service_wasm[] = {
    /* Header */
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    /* Type section */
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    /* Function section */
    0x03, 0x05, 0x04, 0x00, 0x00, 0x00, 0x00,
    /* Memory section */
    0x05, 0x03, 0x01, 0x00, 0x01,
    /* Export section */
    0x07, 0x38, 0x05,
    0x04, 0x69, 0x6E, 0x69, 0x74, 0x00, 0x00,
    0x0A, 0x68, 0x61, 0x6E, 0x64, 0x6C, 0x65, 0x5F, 0x70, 0x70, 0x63, 0x00, 0x01,
    0x0C, 0x68, 0x65, 0x61, 0x6C, 0x74, 0x68, 0x5F, 0x63, 0x68, 0x65, 0x63, 0x6B, 0x00, 0x02,
    0x08, 0x6E, 0x6F, 0x74, 0x69, 0x66, 0x69, 0x65, 0x64, 0x00, 0x03,
    0x06, 0x6D, 0x65, 0x6D, 0x6F, 0x72, 0x79, 0x02, 0x00,
    /* Code section */
    0x0A, 0x0D, 0x04,
    0x02, 0x00, 0x0B,
    0x02, 0x00, 0x0B,
    0x02, 0x00, 0x0B,
    0x02, 0x00, 0x0B,
    /* Custom section "agentos.capabilities" */
    0x00, 0x16, 0x14,
    0x61, 0x67, 0x65, 0x6E, 0x74, 0x6F, 0x73, 0x2E,
    0x63, 0x61, 0x70, 0x61, 0x62, 0x69, 0x6C, 0x69,
    0x74, 0x69, 0x65, 0x73,
    0x01,
};
static const uint32_t g_service_wasm_len = (uint32_t)sizeof(g_service_wasm);

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── Wait for IPC bridge to be initialised by seL4 side ─────────────────── */

static int wait_for_bridge(const ipc_cmd_ring_t *cmd_ring)
{
    int elapsed = 0;
    while (elapsed < BRIDGE_WAIT_TIMEOUT_MS) {
        __sync_synchronize();
        if (cmd_ring->magic == IPC_CMD_MAGIC)
            return 0;
        sleep_ms(BRIDGE_POLL_INTERVAL_MS);
        elapsed += BRIDGE_POLL_INTERVAL_MS;
    }
    return -1;
}

/* ── Write WASM into staging region ─────────────────────────────────────── */

static int write_wasm_to_staging(uint8_t *staging, uint32_t staging_size,
                                  const uint8_t *wasm, uint32_t wasm_len)
{
    if (wasm_len > staging_size) {
        fprintf(stderr, "[agent] WASM too large for staging: %u > %u\n",
                wasm_len, staging_size);
        return -1;
    }
    memcpy(staging, wasm, wasm_len);
    __sync_synchronize();
    fprintf(stderr, "[agent] wrote %u WASM bytes to staging region\n", wasm_len);
    return 0;
}

/* ── Enqueue response in Linux→seL4 ring ────────────────────────────────── */

static int send_wasm_ready(ipc_resp_ring_t *resp_ring, uint32_t seq,
                            uint32_t wasm_len)
{
    uint32_t head = resp_ring->head;
    uint32_t next = (head + 1u) % IPC_RING_DEPTH;

    if (next == resp_ring->tail) {
        fprintf(stderr, "[agent] response ring full\n");
        return -1;
    }

    ipc_resp_t *slot = &resp_ring->resps[head];
    memset(slot, 0, sizeof(*slot));
    slot->seq    = seq;
    slot->status = 0;

    /* Payload: "wasm_ready:<hex_size>" so seL4 side knows how many bytes
     * to read from the staging region. */
    int n = snprintf((char *)slot->payload, IPC_PAYLOAD_LEN,
                     "wasm_ready:%08x", wasm_len);
    slot->payload_len = (uint32_t)(n > 0 ? n : 0);

    __sync_synchronize();
    resp_ring->head = next;
    __sync_synchronize();

    fprintf(stderr, "[agent] enqueued wasm_ready response (seq=%u, size=%u)\n",
            seq, wasm_len);
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const uint8_t *wasm     = g_service_wasm;
    uint32_t       wasm_len = g_service_wasm_len;

    /* Optional: load external WASM binary from argv[1] */
    uint8_t *external_wasm = NULL;
    if (argc >= 2) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) {
            perror("fopen");
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        if (sz <= 0 || (unsigned long)sz > AGENT_STAGING_SIZE) {
            fprintf(stderr, "[agent] WASM file too large or empty\n");
            fclose(f);
            return 1;
        }
        external_wasm = malloc((size_t)sz);
        if (!external_wasm) { fclose(f); return 1; }
        if (fread(external_wasm, 1, (size_t)sz, f) != (size_t)sz) {
            perror("fread"); free(external_wasm); fclose(f); return 1;
        }
        fclose(f);
        wasm     = external_wasm;
        wasm_len = (uint32_t)sz;
        fprintf(stderr, "[agent] loaded external WASM: %s (%u bytes)\n",
                argv[1], wasm_len);
    }

    /* Validate magic before writing */
    if (wasm_len < 8 ||
        wasm[0] != 0x00 || wasm[1] != 0x61 ||
        wasm[2] != 0x73 || wasm[3] != 0x6D) {
        fprintf(stderr, "[agent] WASM magic check failed\n");
        free(external_wasm);
        return 1;
    }

    /* Map shared memory region via /dev/mem */
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("[agent] open /dev/mem");
        free(external_wasm);
        return 1;
    }

    void *base = mmap(NULL, (size_t)IPC_SHMEM_SIZE, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, (off_t)IPC_SHMEM_BASE);
    if (base == MAP_FAILED) {
        perror("[agent] mmap /dev/mem");
        close(fd);
        free(external_wasm);
        return 1;
    }
    close(fd);

    ipc_cmd_ring_t  *cmd_ring  = (ipc_cmd_ring_t  *)((uint8_t *)base + IPC_CMD_RING_OFFSET);
    ipc_resp_ring_t *resp_ring = (ipc_resp_ring_t *)((uint8_t *)base + IPC_RESP_RING_OFFSET);
    uint8_t         *staging   = (uint8_t *)base + AGENT_STAGING_OFFSET;

    /* Step 1: Wait for seL4 side to initialise the bridge */
    fprintf(stderr, "[agent] waiting for IPC bridge (up to %d ms)...\n",
            BRIDGE_WAIT_TIMEOUT_MS);
    if (wait_for_bridge(cmd_ring) < 0) {
        fprintf(stderr, "[agent] timed out waiting for IPC_CMD_MAGIC\n");
        munmap(base, IPC_SHMEM_SIZE);
        free(external_wasm);
        return 1;
    }
    fprintf(stderr, "[agent] IPC bridge ready\n");

    /* Step 2: Write WASM into the staging region */
    if (write_wasm_to_staging(staging, AGENT_STAGING_SIZE, wasm, wasm_len) < 0) {
        munmap(base, IPC_SHMEM_SIZE);
        free(external_wasm);
        return 1;
    }

    /* Step 3: Send wasm_ready response so the seL4 side triggers PROPOSE */
    uint32_t seq = cmd_ring->head;   /* use current cmd head as correlation */
    if (send_wasm_ready(resp_ring, seq, wasm_len) < 0) {
        munmap(base, IPC_SHMEM_SIZE);
        free(external_wasm);
        return 1;
    }

    munmap(base, IPC_SHMEM_SIZE);
    free(external_wasm);

    fprintf(stderr, "[agent] done — WASM deploy initiated\n");
    return 0;
}
