#pragma once
#include <stdint.h>

/* Test-only native Rust IPC contract. No device or guest execution authority.
 * Request: label ECHO, 120 words, MR0 VERSION; MR1..119 arbitrary.
 * Reply: label OK, 120 words, MR0 VERSION;
 *        MR[i] = request[i] XOR (SALT + i), i=1..119.
 * Errors have zero words. Rejected requests must not alter service state.
 * PROBE_BADGE is the root's standard service-id/caller-index badge; the
 * test service echoes it through a separate BADGE request for verification. */
#define NATIVE_RUST_VERSION UINT64_C(1)
#define NATIVE_RUST_ECHO  UINT64_C(0x2e01)
#define NATIVE_RUST_BADGE UINT64_C(0x2e02)
/* HEAP request: two words VERSION, seed (0..255).
 * Reply: six words VERSION, length=2049, checksum, alignment remainder=0,
 * exhaustion_rejected=1, whole_heap_reused=1. Checksum starts at zero and
 * folds each byte ((index*37+seed)&255) with rotate-left-5 then XOR.
 * The service must use alloc::Vec, free it, verify 4096-byte alignment and
 * exhaust/reuse its private 64 KiB heap. Failure returns ERR_HEAP, no words. */
#define NATIVE_RUST_HEAP UINT64_C(0x2e03)
/* EXECUTOR request: one word VERSION. Reply: eight words VERSION,
 * polls_for_two_yielding_tasks=6, initial_completions=2, full_rejected=1,
 * zero_budget_polls=0, stale_cancel_rejected=1, final_completions=3,
 * remaining_tasks=0. Each task yields twice via its waker before completing.
 * Failure returns ERR_EXECUTOR with no words. */
#define NATIVE_RUST_EXECUTOR UINT64_C(0x2e04)
/* NETWORK request: one word VERSION. Reply: hardware state=1, verified ARP
 * replies=3, notification wake count >=3. Each request uses the assigned source
 * IP and waits on its receive-only notification before reading queue data. */
#define NATIVE_RUST_NETWORK UINT64_C(0x2e05)
#define NATIVE_RUST_OK UINT64_C(0)
#define NATIVE_RUST_ERR_OPCODE UINT64_C(1)
#define NATIVE_RUST_ERR_LENGTH UINT64_C(2)
#define NATIVE_RUST_ERR_VERSION UINT64_C(3)
#define NATIVE_RUST_ERR_HEAP UINT64_C(4)
#define NATIVE_RUST_ERR_EXECUTOR UINT64_C(5)
#define NATIVE_RUST_WORDS 120u
#define NATIVE_RUST_SALT UINT64_C(0x5a5a5a5a5a5a5a5a)
#define NATIVE_RUST_PROBE_ENDPOINT 16u
