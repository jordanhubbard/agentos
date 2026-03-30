/*
 * ed25519_verify.h — Minimal Ed25519 signature verification for agentOS
 *
 * Pure C implementation suitable for bare-metal / Microkit (no malloc, no libc).
 * Based on the public domain ref10 algorithm by Daniel J. Bernstein et al.
 *
 * Only verification is implemented — signing happens offline via the host
 * toolchain (tools/sign_wasm.py or similar).
 *
 * API:
 *   int ed25519_verify(const uint8_t sig[64],
 *                      const uint8_t *msg, uint32_t msg_len,
 *                      const uint8_t pubkey[32]);
 *   Returns 0 on valid signature, non-zero on failure.
 *
 *   void sha512(const uint8_t *msg, uint32_t len, uint8_t hash[64]);
 *   SHA-512 hash used internally by Ed25519.
 */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Verify an Ed25519 signature.
 * Returns 0 if the signature is valid, non-zero otherwise.
 */
int ed25519_verify(const uint8_t sig[64],
                   const uint8_t *msg, uint32_t msg_len,
                   const uint8_t pubkey[32]);

/*
 * SHA-512 hash function (used internally by Ed25519, exposed for module hashing).
 */
void sha512(const uint8_t *msg, uint32_t len, uint8_t hash[64]);

#ifdef __cplusplus
}
#endif
