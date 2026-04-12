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

/*
 * Curve25519 Diffie-Hellman.
 * raw_shared[32]      — output: shared secret (u-coordinate of scalar*point)
 * your_secret_key[32] — input:  scalar (clamped internally)
 * their_public_key[32]— input:  peer's u-coordinate
 *
 * Use with basepoint = {9, 0, 0, ..., 0} for public-key derivation.
 */
void crypto_x25519(uint8_t       raw_shared[32],
                   const uint8_t your_secret_key[32],
                   const uint8_t their_public_key[32]);

/*
 * ChaCha20-Poly1305 AEAD encryption (XChaCha20 variant, 24-byte nonce).
 * cipher_text[plain_size]  — output: encrypted payload (same length as input)
 * mac[16]                  — output: Poly1305 authentication tag
 * key[32]                  — input:  symmetric key
 * nonce[24]                — input:  unique nonce (XChaCha20)
 * ad / ad_size             — additional authenticated data (may be NULL/0)
 * plain_text / plain_size  — input:  plaintext
 */
void crypto_aead_lock(uint8_t       *cipher_text,
                      uint8_t        mac[16],
                      const uint8_t  key[32],
                      const uint8_t  nonce[24],
                      const uint8_t *ad,       size_t ad_size,
                      const uint8_t *plain_text, size_t plain_size);

/*
 * ChaCha20-Poly1305 AEAD decryption (XChaCha20 variant, 24-byte nonce).
 * plain_text[cipher_size]  — output: decrypted payload
 * mac[16]                  — input:  Poly1305 tag to verify
 * key[32]                  — input:  symmetric key
 * nonce[24]                — input:  unique nonce
 * ad / ad_size             — additional authenticated data (may be NULL/0)
 * cipher_text / cipher_size— input:  ciphertext
 *
 * Returns 0 on success (MAC verified), -1 if authentication failed.
 * Wipes plain_text on failure.
 */
int  crypto_aead_unlock(uint8_t       *plain_text,
                        const uint8_t  mac[16],
                        const uint8_t  key[32],
                        const uint8_t  nonce[24],
                        const uint8_t *ad,        size_t ad_size,
                        const uint8_t *cipher_text, size_t cipher_size);

#ifdef __cplusplus
}
#endif

#endif /* MONOCYPHER_H */
