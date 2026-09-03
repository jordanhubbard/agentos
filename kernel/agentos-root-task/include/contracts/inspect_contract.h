/*
 * Inspect IPC contract (read-only)
 *
 * Packed snapshot of memory, threads (PDs), and hardware attributes.
 * The live ABI and host tests live in platform/include/platform/inspect.h.
 * This header is the future seL4 IPC view of the same structs.
 *
 * No PD implements this yet. Do not add MSG_* opcodes to agentos.h until
 * an inspect reporter exists. That reporter is not TCB: no device frames,
 * no IRQs. term_server is museum — session I/O is serial_virt, not this
 * contract.
 *
 * Opcodes (local until a PD is registered):
 *   AOS_INSPECT_OP_SNAPSHOT  full snapshot
 *   AOS_INSPECT_OP_MEMORY    memory subsection
 *   AOS_INSPECT_OP_THREADS   thread table
 *   AOS_INSPECT_OP_HARDWARE  hardware subsection
 *
 * Invariants:
 *   - Read-only. No mutate, compose, or spawn opcodes here.
 *   - Reply payload is aos_inspect_snapshot_t (platform/inspect.h).
 *   - version must be AOS_INSPECT_VERSION.
 *   - thread_count <= AOS_INSPECT_MAX_THREADS.
 */

#pragma once

#include <stdint.h>

#define AOS_INSPECT_OP_SNAPSHOT   1u
#define AOS_INSPECT_OP_MEMORY     2u
#define AOS_INSPECT_OP_THREADS    3u
#define AOS_INSPECT_OP_HARDWARE   4u

typedef struct __attribute__((packed)) aos_inspect_req {
    uint32_t opcode;
    uint32_t flags;
} aos_inspect_req_t;

typedef struct __attribute__((packed)) aos_inspect_reply_hdr {
    uint32_t opcode;
    int32_t  status;
} aos_inspect_reply_hdr_t;
