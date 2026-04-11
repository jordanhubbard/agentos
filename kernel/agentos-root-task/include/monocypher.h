/*
 * monocypher.h — Ed25519 signature API for agentOS
 *
 * Derived from Monocypher 4.0.2 (public domain, Loup Vaillant).
 * No libc, no dynamic allocation.  Suitable for bare-metal seL4 kernel PDs.
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#ifndef MONOCYPHER_H
#define MONOCYPHER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ed25519 signature verification.
 * sig[64]  — R || S signature bytes
 * msg      — message buffer (arbitrary length)
 * msg_len  — length of msg in bytes
 * pk[32]   — Ed25519 public key
 *
 * Returns 0 on success (signature valid), -1 on failure.
 */
int crypto_ed25519_check(const uint8_t *sig,
                         const uint8_t *msg, size_t msg_len,
                         const uint8_t *pk);

/*
 * Ed25519 public-key derivation.
 * pk[32]  — output: public key
 * sk[32]  — input:  32-byte seed (private key scalar material)
 */
void crypto_ed25519_public_key(uint8_t *pk, const uint8_t *sk);

/*
 * Ed25519 signing.
 * sig[64]  — output: signature
 * sk[32]   — 32-byte seed
 * pk[32]   — public key (from crypto_ed25519_public_key)
 * msg      — message buffer
 * msg_len  — length of msg in bytes
 */
void crypto_ed25519_sign(uint8_t *sig,
                         const uint8_t *sk, const uint8_t *pk,
                         const uint8_t *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif

#endif /* MONOCYPHER_H */
