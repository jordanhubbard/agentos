/*
 * ipc_bridge.h — seL4 ↔ Linux VM IPC bridge
 *
 * Defines the shared-memory command/response rings used by the IPC bridge
 * embedded in the debug_bridge protection domain.  Native seL4 PDs enqueue
 * commands into ipc_cmd_ring_t and poll responses from ipc_resp_ring_t;
 * the Linux guest (via a virtIO console or /dev/ipc_shmem driver) reads
 * commands and writes responses to the same region.
 *
 * Physical layout inside the IPC shmem MR (base IPC_SHMEM_BASE):
 *   [IPC_CMD_RING_OFFSET  .. IPC_CMD_RING_OFFSET  + sizeof(ipc_cmd_ring_t)]
 *       seL4→Linux command ring
 *   [IPC_RESP_RING_OFFSET .. IPC_RESP_RING_OFFSET + sizeof(ipc_resp_ring_t)]
 *       Linux→seL4 response ring
 *
 * All ring accesses are protected by compiler memory barriers.  Atomicity is
 * not required because only one writer exists per direction (seL4 side writes
 * commands; Linux side writes responses).
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#pragma once
#ifndef IPC_BRIDGE_H
#define IPC_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

/* ── Address-space constants ──────────────────────────────────────────────── */

/* Base vaddr of the IPC/GPU shmem MR in the debug_bridge PD */
#define IPC_SHMEM_BASE          0x4000000UL

/* Byte offsets within the MR for the two rings */
#define IPC_CMD_RING_OFFSET     0x1000UL   /* seL4→Linux command ring */
#define IPC_RESP_RING_OFFSET    0x2000UL   /* Linux→seL4 response ring */

/* ── Ring magic values ────────────────────────────────────────────────────── */

#define IPC_CMD_MAGIC           0x49504343UL  /* "IPCC" */
#define IPC_RESP_MAGIC          0x49504352UL  /* "IPCR" */

/* ── Operation codes ──────────────────────────────────────────────────────── */

#define IPC_OP_EXEC             0x01  /* exec a command in the Linux guest */
#define IPC_OP_WRITE            0x02  /* write a file in the Linux guest */
#define IPC_OP_READ             0x03  /* read a file from the Linux guest */
#define IPC_OP_PING             0x04  /* liveness check */
#define IPC_OP_SPAWN            0x05  /* spawn a named agent process */
#define IPC_OP_SIGNAL           0x06  /* send signal to a guest process */

/* ── Ring geometry ────────────────────────────────────────────────────────── */

#define IPC_RING_DEPTH          64    /* slots in each ring */
#define IPC_PAYLOAD_LEN         128   /* inline payload bytes per entry */

/* ── Command entry (seL4 → Linux) ────────────────────────────────────────── */

typedef struct {
    uint32_t seq;                      /* monotonically increasing sequence number */
    uint32_t op;                       /* IPC_OP_* */
    uint32_t vm_slot;                  /* target VM slot (0–3) */
    uint32_t payload_len;              /* valid bytes in payload[] */
    uint8_t  payload[IPC_PAYLOAD_LEN]; /* inline payload (paths, args, …) */
} ipc_cmd_t;

/* ── Response entry (Linux → seL4) ──────────────────────────────────────── */

typedef struct {
    uint32_t seq;                      /* matches the originating cmd seq */
    uint32_t status;                   /* 0 = ok, non-zero = errno */
    uint32_t payload_len;              /* valid bytes in payload[] */
    uint32_t _pad;
    uint8_t  payload[IPC_PAYLOAD_LEN]; /* inline response data */
} ipc_resp_t;

/* ── Command ring header ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t  magic;                   /* IPC_CMD_MAGIC */
    uint32_t  head;                    /* seL4 write index (producer) */
    uint32_t  tail;                    /* Linux read index (consumer) */
    uint32_t  _pad;
    ipc_cmd_t cmds[IPC_RING_DEPTH];
} ipc_cmd_ring_t;

/* ── Response ring header ────────────────────────────────────────────────── */

typedef struct {
    uint32_t   magic;                  /* IPC_RESP_MAGIC */
    uint32_t   head;                   /* Linux write index (producer) */
    uint32_t   tail;                   /* seL4 read index (consumer) */
    uint32_t   _pad;
    ipc_resp_t resps[IPC_RING_DEPTH];
} ipc_resp_ring_t;

/* ── Response dispatch callback ──────────────────────────────────────────── */

/*
 * Optional callback invoked by ipc_bridge_notified() for each response
 * drained from the ring.  Register with ipc_bridge_set_resp_cb().
 * The callback runs in the notified() context (no PPC in flight).
 */
typedef void (*ipc_resp_cb_t)(const ipc_resp_t *resp, void *cookie);

/* ── API ─────────────────────────────────────────────────────────────────── */

/*
 * ipc_bridge_init()
 *
 * Write magic values, zero heads/tails, and record shmem_vaddr for later
 * ring pointer arithmetic.  Call once from debug_bridge init().
 *
 * shmem_vaddr: virtual address of the IPC shmem MR base in this PD.
 */
void ipc_bridge_init(uintptr_t shmem_vaddr);

/*
 * ipc_bridge_send_cmd()
 *
 * Enqueue a command in the seL4→Linux ring.  Non-blocking; returns -1 if
 * the ring is full.  On success writes the assigned sequence number into
 * *out_seq (if non-NULL) and returns 0.
 *
 * op:          IPC_OP_* operation code
 * vm_slot:     target VM slot (0–3)
 * payload:     pointer to payload bytes (may be NULL if payload_len == 0)
 * payload_len: number of bytes to copy (clamped to IPC_PAYLOAD_LEN)
 * out_seq:     receives the assigned sequence number
 */
int ipc_bridge_send_cmd(uint32_t op, uint32_t vm_slot,
                        const void *payload, uint32_t payload_len,
                        uint32_t *out_seq);

/*
 * ipc_bridge_poll_resp()
 *
 * Non-blocking poll of the Linux→seL4 response ring for a reply to seq.
 *
 * Returns:
 *   1  — matching response found; *out_resp populated, entry consumed
 *   0  — no matching response yet (still pending)
 *  -1  — error (bridge not initialised, or invalid out_resp pointer)
 *
 * seq:      sequence number returned by ipc_bridge_send_cmd()
 * out_resp: caller-supplied buffer to receive the response
 */
int ipc_bridge_poll_resp(uint32_t seq, ipc_resp_t *out_resp);

/*
 * ipc_bridge_notified()
 *
 * Called from the debug_bridge notified() handler when the IPC shmem
 * notification channel fires.  Drains all pending responses from the
 * Linux→seL4 ring and invokes the registered callback (if any) for each.
 */
void ipc_bridge_notified(void);

/*
 * ipc_bridge_set_resp_cb()
 *
 * Register a callback to be invoked for each response drained by
 * ipc_bridge_notified().  Pass NULL to clear.  cookie is forwarded
 * unchanged to each cb() call.
 */
void ipc_bridge_set_resp_cb(ipc_resp_cb_t cb, void *cookie);

#endif /* IPC_BRIDGE_H */
