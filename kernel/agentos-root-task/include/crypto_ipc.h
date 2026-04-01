/*
 * crypto_ipc.h — seL4 PD-to-PD encrypted IPC channel
 *
 * Provides ChaCha20-Poly1305 AEAD encryption for shared-memory ring entries.
 * PDs negotiate a shared symmetric key via X25519 ECDH on first contact
 * (brokered through init_agent).
 *
 * Architecture:
 *   1. PD A calls crypto_ipc_init_producer(channel_id) → generates ephemeral
 *      X25519 keypair, publishes public key via OP_CRYPTO_HELLO to init_agent.
 *   2. init_agent broadcasts public keys; PD B calls crypto_ipc_init_consumer().
 *   3. Both sides compute X25519 shared secret; derive ChaCha20 key via HKDF-SHA256.
 *   4. Encrypt: crypto_ipc_seal(plaintext, len, nonce, key, ciphertext)
 *      → encrypted ring entry prefixed with 12-byte nonce + 16-byte Poly1305 tag.
 *   5. Decrypt: crypto_ipc_open(ciphertext, len, key, plaintext)
 *
 * Wire format for encrypted ring entries:
 *   [0..11]    nonce (12 bytes, monotonic counter + random salt)
 *   [12..27]   Poly1305 tag (16 bytes)
 *   [28..end]  ChaCha20-encrypted payload
 *
 * This is a prototype implementation with a simplified X25519 using
 * Curve25519 arithmetic suitable for bare-metal seL4 PDs (no libc,
 * no dynamic allocation). In production, use monocypher or libsodium.
 *
 * Reference: RFC 8439 (ChaCha20-Poly1305), RFC 7748 (X25519).
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#pragma once
#ifndef CRYPTO_IPC_H
#define CRYPTO_IPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Key sizes ────────────────────────────────────────────────────────── */

#define CRYPTO_IPC_KEY_SIZE     32  /* ChaCha20 key: 256 bits */
#define CRYPTO_IPC_NONCE_SIZE   12  /* ChaCha20 nonce: 96 bits */
#define CRYPTO_IPC_TAG_SIZE     16  /* Poly1305 tag: 128 bits */
#define CRYPTO_IPC_OVERHEAD     (CRYPTO_IPC_NONCE_SIZE + CRYPTO_IPC_TAG_SIZE)
#define CRYPTO_IPC_X25519_KEY   32  /* Curve25519 public/private key */

/* ── Channel state ────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  key[CRYPTO_IPC_KEY_SIZE];   /* negotiated symmetric key */
    uint64_t nonce_counter;              /* monotonic nonce counter */
    uint8_t  nonce_salt[4];              /* per-channel random salt */
    bool     ready;                      /* true after key negotiation */
} CryptoIpcChannel;

/* ── X25519 key exchange state ────────────────────────────────────────── */

typedef struct {
    uint8_t private_key[CRYPTO_IPC_X25519_KEY];
    uint8_t public_key[CRYPTO_IPC_X25519_KEY];
} CryptoIpcKeyPair;

/* ── API ──────────────────────────────────────────────────────────────── */

/*
 * Generate an ephemeral X25519 keypair from a 32-byte seed.
 * The seed should come from a hardware RNG or the seL4 time counter.
 */
void crypto_ipc_keygen(CryptoIpcKeyPair *kp, const uint8_t seed[32]);

/*
 * Compute X25519 shared secret from our private key and peer's public key.
 * Writes 32 bytes to shared_secret.
 */
void crypto_ipc_x25519(uint8_t shared_secret[32],
                        const uint8_t private_key[32],
                        const uint8_t peer_public_key[32]);

/*
 * Derive a ChaCha20 symmetric key from a shared secret using HKDF-SHA256.
 * info: a short ASCII context string (e.g., "agentOS-ipc-v1").
 */
void crypto_ipc_derive_key(uint8_t key_out[CRYPTO_IPC_KEY_SIZE],
                            const uint8_t shared_secret[32],
                            const char *info);

/*
 * Initialise a CryptoIpcChannel from a derived key and a 4-byte random salt.
 */
void crypto_ipc_channel_init(CryptoIpcChannel *ch,
                              const uint8_t key[CRYPTO_IPC_KEY_SIZE],
                              const uint8_t salt[4]);

/*
 * Seal plaintext into ciphertext.
 * ciphertext_buf must be at least plaintext_len + CRYPTO_IPC_OVERHEAD bytes.
 * Returns total bytes written (plaintext_len + CRYPTO_IPC_OVERHEAD).
 */
size_t crypto_ipc_seal(CryptoIpcChannel *ch,
                        const uint8_t *plaintext, size_t plaintext_len,
                        uint8_t *ciphertext_buf);

/*
 * Open (decrypt + verify) a sealed ciphertext.
 * plaintext_buf must be at least ciphertext_len - CRYPTO_IPC_OVERHEAD bytes.
 * Returns plaintext length on success, 0 on authentication failure.
 */
size_t crypto_ipc_open(const CryptoIpcChannel *ch,
                        const uint8_t *ciphertext, size_t ciphertext_len,
                        uint8_t *plaintext_buf);

/*
 * Typed ring buffer helpers: encrypt before enqueue, decrypt after dequeue.
 * These wrap ringbuf.h for encrypted entry IPC.
 */
#include "ringbuf.h"

/*
 * Enqueue an encrypted entry.
 * slot_size: sizeof the ring slot (plaintext_len + CRYPTO_IPC_OVERHEAD).
 * Returns true on success.
 */
bool crypto_ipc_ring_enqueue(CryptoIpcChannel *ch,
                              ringbuf_hdr_t *ring, void *slots,
                              uint32_t slot_size,
                              const void *plaintext, uint32_t plaintext_len);

/*
 * Dequeue and decrypt one entry.
 * Returns plaintext length on success, 0 if empty or auth failed.
 */
uint32_t crypto_ipc_ring_dequeue(const CryptoIpcChannel *ch,
                                  ringbuf_hdr_t *ring, const void *slots,
                                  uint32_t slot_size,
                                  void *plaintext_buf, uint32_t plaintext_buf_size);

#endif /* CRYPTO_IPC_H */
