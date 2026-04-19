/*
 * contracts/entropy-service/interface.h — Entropy/RNG Generic Device Interface
 *
 * // STATUS: IMPLEMENTED
 *
 * This is the canonical contract for the entropy-service device service in agentOS.
 * The concrete implementation is backed by the monocypher-based crypto primitives
 * in kernel/agentos-root-task/src/monocypher.c and crypto_ipc.c.  The entropy
 * source is the hardware RNG (platform-specific: ARM TRNG via MRS S3_3_C2_C4_0,
 * x86 RDRAND, or a seeded ChaCha20 DRBG when hardware is unavailable).
 *
 * The entropy service is the ONLY authorized source of cryptographic randomness
 * in agentOS.  Guest OSes and VMMs MUST NOT read hardware RNG registers directly
 * (/dev/hwrng, RDRAND, ARM TRNG MMIO).  All randomness must flow through this
 * service to ensure proper mixing, auditing, and rate limiting.
 *
 * IPC transport:
 *   - Protected procedure call (PPC) via seL4 Microkit.
 *   - MR0 = opcode (ENTROPY_SVC_OP_*)
 *   - MR1..MR3 = arguments (see per-op comments below)
 *   - Reply: MR0 = status (ENTROPY_SVC_ERR_*), MR1..MR3 = result fields
 *
 * Shared-memory entropy buffer:
 *   For requests > ENTROPY_SVC_INLINE_MAX bytes the entropy service writes
 *   random data into a shared memory region (entropy_shmem) mapped into both
 *   the service PD and the requesting client PD.  For small requests (up to
 *   ENTROPY_SVC_INLINE_MAX bytes) the data is returned directly in MRs 1..4.
 *
 * Security note:
 *   This implementation has not been FIPS 140-3 validated.  For production
 *   key generation, the caller must mix ENTROPY_SVC output with additional
 *   application entropy (timestamps, hardware identifiers) before use.
 *   See monocypher.h for the underlying crypto primitives.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ── Interface version ──────────────────────────────────────────────────── */
#define ENTROPY_SVC_INTERFACE_VERSION   1

/* ── Geometry / limits ──────────────────────────────────────────────────── */

/*
 * Inline delivery: GET_BYTES requests of up to ENTROPY_SVC_INLINE_MAX bytes
 * are returned in MR1..MR4 (16 bytes) without requiring a shared-memory
 * mapping.  This avoids the cost of a memory region lookup for the common case
 * of generating a small nonce or key.
 */
#define ENTROPY_SVC_INLINE_MAX          16u   /* bytes returnable in MRs 1..4 */

/*
 * Bulk delivery upper bound per call.  Callers requesting more than
 * ENTROPY_SVC_BULK_MAX bytes must issue multiple calls.
 */
#define ENTROPY_SVC_BULK_MAX            4096u  /* bytes per single GET_BYTES call */

/* Quality levels for GET_BYTES (MR2 = quality) */
#define ENTROPY_SVC_QUALITY_NONCE       0u   /* fast: suitable for nonces only */
#define ENTROPY_SVC_QUALITY_KEY         1u   /* strong: conditioned hardware RNG */
#define ENTROPY_SVC_QUALITY_FIPS        2u   /* FIPS DRBG (reserved, not yet impl) */

/* ── Opcodes (MR0) ──────────────────────────────────────────────────────── */

/*
 * ENTROPY_SVC_OP_GET_BYTES (0xE0)
 * Request N bytes of random data from the entropy service.
 *
 * For len <= ENTROPY_SVC_INLINE_MAX the bytes are returned in MR1..MR4
 * and buf_offset is ignored.
 *
 * For len > ENTROPY_SVC_INLINE_MAX the entropy service writes into the
 * shared entropy_shmem at buf_offset; the caller reads from there after
 * the reply.
 *
 * Request:
 *   MR1 = len             — number of random bytes requested
 *                           (1..ENTROPY_SVC_BULK_MAX)
 *   MR2 = quality         — ENTROPY_SVC_QUALITY_*
 *   MR3 = buf_offset      — byte offset in entropy_shmem for bulk results
 *                           (ignored when len <= ENTROPY_SVC_INLINE_MAX)
 * Reply (inline, len <= 16):
 *   MR0 = status
 *   MR1 = bytes_lo        — bytes 0..3  as a little-endian uint32
 *   MR2 = bytes_4_7       — bytes 4..7
 *   MR3 = bytes_8_11      — bytes 8..11
 *   MR4 = bytes_12_15     — bytes 12..15  (zero-padded if len < 16)
 *
 * Reply (bulk, len > 16):
 *   MR0 = status
 *   MR1 = bytes_written   — actual bytes written into entropy_shmem
 *   MR2 = buf_offset      — echoed for caller convenience
 */
#define ENTROPY_SVC_OP_GET_BYTES        0xE0u

/* ── Error / status codes (MR0 in replies) ──────────────────────────────── */
#define ENTROPY_SVC_ERR_OK              0u   /* success */
#define ENTROPY_SVC_ERR_INVAL           1u   /* len == 0 or > ENTROPY_SVC_BULK_MAX */
#define ENTROPY_SVC_ERR_NODEV           2u   /* hardware RNG unavailable */
#define ENTROPY_SVC_ERR_PERM            3u   /* capability check failed */
#define ENTROPY_SVC_ERR_QUALITY         4u   /* requested quality level not available */
#define ENTROPY_SVC_ERR_TIMEOUT         5u   /* hardware RNG stalled */

/* ── Request / reply structs ─────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;        /* ENTROPY_SVC_OP_* */
    uint32_t len;           /* bytes requested */
    uint32_t quality;       /* ENTROPY_SVC_QUALITY_* */
    uint32_t buf_offset;    /* shmem offset for bulk results */
} entropy_svc_req_t;

/*
 * entropy_svc_reply_t covers both the inline and bulk reply cases.
 * For inline (len <= 16): data[] contains the random bytes.
 * For bulk (len > 16): bytes_written and buf_offset are set; data[] is zero.
 */
typedef struct __attribute__((packed)) {
    uint32_t status;         /* ENTROPY_SVC_ERR_* */
    union {
        struct __attribute__((packed)) {
            uint32_t bytes_lo;      /* MR1: bytes  0..3  */
            uint32_t bytes_4_7;     /* MR2: bytes  4..7  */
            uint32_t bytes_8_11;    /* MR3: bytes  8..11 */
            uint32_t bytes_12_15;   /* MR4: bytes 12..15 */
        } inline_data;
        struct __attribute__((packed)) {
            uint32_t bytes_written;
            uint32_t buf_offset;
        } bulk;
    };
} entropy_svc_reply_t;
