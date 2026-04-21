# EntropyService — Cryptographic Randomness Service Contract

## Overview

EntropyService is the sole authorized source of cryptographic randomness in
agentOS.  Guest OSes and VMMs must not read hardware RNG registers directly
(`/dev/hwrng`, `RDRAND`, ARM TRNG MMIO).  All randomness flows through this
service for proper mixing, auditing, and rate limiting.

Hardware entropy sources (in priority order):
1. ARM TRNG via `MRS S3_3_C2_C4_0`
2. x86 `RDRAND`
3. Seeded ChaCha20 DRBG (fallback when hardware is unavailable)

The implementation uses monocypher primitives from
`kernel/agentos-root-task/src/monocypher.c`.

## Status

**IMPLEMENTED.** The backing implementation is in `crypto_ipc.c` (the
`crypto_ipc` kernel module).

## Protection Domain

EntropyService is typically collocated with the monitor PD.  Callers receive
a PPC capability to its endpoint at initialization time.

## Operations

| Opcode | Description |
|--------|-------------|
| `ENTROPY_SVC_OP_GET_BYTES`  | Fill buffer with random bytes |
| `ENTROPY_SVC_OP_GET_UINT32` | Return a single random uint32 |
| `ENTROPY_SVC_OP_GET_UINT64` | Return a single random uint64 |
| `ENTROPY_SVC_OP_SEED`       | Inject additional entropy (privileged) |
| `ENTROPY_SVC_OP_STATUS`     | Health check and entropy pool stats |

## Source Files

- `contracts/entropy-service/interface.h` — canonical IPC contract
- `kernel/agentos-root-task/src/crypto_ipc.c` — implementation
- `kernel/agentos-root-task/include/crypto_ipc.h` — internal header
- `kernel/agentos-root-task/include/monocypher.h` — crypto primitives
