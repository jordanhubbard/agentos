/*
 * sha256_mini.h — Minimal software SHA-256 (FIPS 180-4)
 *
 * Self-contained, no-libc, no dynamic allocation.
 * Suitable for bare-metal seL4 protection domains.
 *
 * Public domain / CC0 — based on the B. Poettering reference implementation.
 *
 * Usage:
 *   uint8_t digest[32];
 *   sha256_mini(data, data_len, digest);
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * sha256_mini — compute SHA-256 digest of `data` (len bytes).
 * Writes exactly 32 bytes to `digest`.
 */
void sha256_mini(const uint8_t *data, uint32_t len, uint8_t digest[32]);
