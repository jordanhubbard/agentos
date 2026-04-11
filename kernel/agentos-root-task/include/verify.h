#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Ed25519 + SHA-256 WASM module verifier
 *
 * VIBE_VERIFY_MODE compile flag:
 *   -DVIBE_VERIFY_MODE=1  → production: reject loads with bad/missing sig (return false)
 *   -DVIBE_VERIFY_MODE=0  → dev mode: log warning but return true
 *   Default (undefined):  → dev mode
 *
 * Returns true if signature is valid (or dev mode with missing/bad sig).
 * Returns false only in production mode with bad/missing signature.
 */
bool vibe_verify_module(const uint8_t *wasm, size_t len, const uint8_t *trusted_pubkey);

/* Raw Ed25519 verification over an arbitrary-length message.
 * sig[64]: R||S signature bytes; msg/msg_len: message; pk[32]: public key.
 * Returns 0 on success, -1 on failure.
 */
int ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t msg_len,
                   const uint8_t pk[32]);

/*
 * verify_capabilities_manifest - check that the agentos.cap_signature section
 * (SHA-256 digest, 32 bytes) matches a fresh SHA-256 of the agentos.capabilities
 * section bytes.
 *
 * Returns  0  success — manifest is authentic.
 *         -1  required sections missing (no capabilities manifest present).
 *         -2  hash mismatch — manifest has been tampered.
 */
int verify_capabilities_manifest(const uint8_t *wasm, size_t wasm_len);
