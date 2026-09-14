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
#define NATIVE_RUST_OK UINT64_C(0)
#define NATIVE_RUST_ERR_OPCODE UINT64_C(1)
#define NATIVE_RUST_ERR_LENGTH UINT64_C(2)
#define NATIVE_RUST_ERR_VERSION UINT64_C(3)
#define NATIVE_RUST_WORDS 120u
#define NATIVE_RUST_SALT UINT64_C(0x5a5a5a5a5a5a5a5a)
#define NATIVE_RUST_PROBE_ENDPOINT 16u
