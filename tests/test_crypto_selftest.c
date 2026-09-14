/*
 * test_crypto_selftest.c — host-side validation of agentOS Ed25519 selftest
 *
 * Proves three things required by agentos-c7i:
 *   1. Ed25519 known-GOOD RFC 8032 test vector verifies (returns 0).
 *   2. Ed25519 known-BAD vectors (corrupted sig / msg / pk) are REJECTED.
 *   3. The fatal selftest gate (crypto_selftest()) returns 0 on success and
 *      nonzero on a forced failure, and the test harness ABORTS (nonzero exit)
 *      when the gate fails — mirroring the boot-time hard-fail behaviour.
 *
 * The selftest gate exercised here is the SAME function the controller calls
 * at boot (services/legacy-pds/verify.c :: crypto_selftest), which on
 * the target panics via crypto_selftest_panic() if any vector fails.
 *
 * Build:
 *   cc -o /tmp/test_crypto_selftest \
 *       tests/test_crypto_selftest.c \
 *       services/legacy-pds/verify.c \
 *       libs/pd-support/ed25519_verify.c \
 *       libs/pd-support/monocypher.c \
 *       -I tests -I kernel/agentos-root-task/include \
 *       -DAGENTOS_TEST_HOST
 * Run:
 *   /tmp/test_crypto_selftest    # exits 0 on success, nonzero on failure
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "monocypher.h"

/* verify.c logs via log_drain_write() (inline in agentos.h), which references
 * this seL4cp setvar global.  Host builds have no log ring mapped, so define
 * it as 0 — log_drain_write() then becomes a no-op. */
uintptr_t log_drain_rings_vaddr = 0;

/* The fatal selftest gate (verify.c). Returns 0 on success, nonzero on failure. */
int crypto_selftest(void);
/* Test-only entry: run the gate's verify step against caller-supplied vectors. */
int crypto_selftest_with_vectors(const uint8_t sig[64], const uint8_t *msg,
                                 size_t msg_len, const uint8_t pk[32]);

/* ── TAP-ish test infrastructure ──────────────────────────────────────────── */

static int g_failures = 0;
#define CHECK(cond, name) do {                                               \
    if (cond) { printf("ok - %s\n", name); }                                 \
    else { printf("not ok - %s (%s:%d)\n", name, __FILE__, __LINE__);        \
           g_failures++; }                                                   \
} while (0)

/* ──────────────────────────────────────────────────────────────────────────
 * RFC 8032 Section 7.1, Ed25519 TEST 2 (1-octet message)
 * ──────────────────────────────────────────────────────────────────────── */
static const uint8_t rfc_sk[32] = {
    0x4c,0xcd,0x08,0x9b,0x28,0xff,0x96,0xda,0x9d,0xb6,0xc3,0x46,0xec,0x11,0x4e,0x0f,
    0x5b,0x8a,0x31,0x9f,0x35,0xab,0xa6,0x24,0xda,0x8c,0xf6,0xed,0x4f,0xb8,0xa6,0xfb,
};
static const uint8_t rfc_pk[32] = {
    0x3d,0x40,0x17,0xc3,0xe8,0x43,0x89,0x5a,0x92,0xb7,0x0a,0xa7,0x4d,0x1b,0x7e,0xbc,
    0x9c,0x98,0x2c,0xcf,0x2e,0xc4,0x96,0x8c,0xc0,0xcd,0x55,0xf1,0x2a,0xf4,0x66,0x0c,
};
static const uint8_t rfc_msg[1] = { 0x72 };
static const uint8_t rfc_sig[64] = {
    0x92,0xa0,0x09,0xa9,0xf0,0xd4,0xca,0xb8,0x72,0x0e,0x82,0x0b,0x5f,0x64,0x25,0x40,
    0xa2,0xb2,0x7b,0x54,0x16,0x50,0x3f,0x8f,0xb3,0x76,0x22,0x23,0xeb,0xdb,0x69,0xda,
    0x08,0x5a,0xc1,0xe4,0x3e,0x15,0x99,0x6e,0x45,0x8f,0x36,0x13,0xd0,0xf1,0x1d,0x8c,
    0x38,0x7b,0x2e,0xae,0xb4,0x30,0x2a,0xee,0xb0,0x0d,0x29,0x16,0x12,0xbb,0x0c,0x00,
};

/* ── Tests ────────────────────────────────────────────────────────────────── */

static void test_known_good(void)
{
    int rc = crypto_ed25519_check(rfc_sig, rfc_msg, sizeof(rfc_msg), rfc_pk);
    CHECK(rc == 0, "RFC8032 known-good vector verifies");

    /* Public key derived from the seed must match the published pk. */
    uint8_t pk[32];
    crypto_ed25519_public_key(pk, rfc_sk);
    CHECK(memcmp(pk, rfc_pk, 32) == 0, "public key derivation matches RFC8032 pk");

    /* Round-trip: signing the message reproduces the published signature. */
    uint8_t sig[64];
    crypto_ed25519_sign(sig, rfc_sk, rfc_pk, rfc_msg, sizeof(rfc_msg));
    CHECK(memcmp(sig, rfc_sig, 64) == 0, "sign reproduces RFC8032 signature");
}

static void test_known_bad(void)
{
    uint8_t sig[64];

    /* Corrupted signature (flip a bit in R) must be rejected. */
    memcpy(sig, rfc_sig, 64);
    sig[0] ^= 0x01;
    CHECK(crypto_ed25519_check(sig, rfc_msg, sizeof(rfc_msg), rfc_pk) != 0,
          "corrupted-R signature is REJECTED");

    /* Corrupted signature (flip a bit in S) must be rejected. */
    memcpy(sig, rfc_sig, 64);
    sig[40] ^= 0x01;
    CHECK(crypto_ed25519_check(sig, rfc_msg, sizeof(rfc_msg), rfc_pk) != 0,
          "corrupted-S signature is REJECTED");

    /* Tampered message must be rejected. */
    uint8_t bad_msg[1] = { 0x73 };
    CHECK(crypto_ed25519_check(rfc_sig, bad_msg, sizeof(bad_msg), rfc_pk) != 0,
          "tampered message is REJECTED");

    /* Wrong public key must be rejected. */
    uint8_t bad_pk[32];
    memcpy(bad_pk, rfc_pk, 32);
    bad_pk[5] ^= 0x01;
    CHECK(crypto_ed25519_check(rfc_sig, rfc_msg, sizeof(rfc_msg), bad_pk) != 0,
          "wrong public key is REJECTED");

    /* Malleable S (S >= l, top bits set) must be rejected outright. */
    memcpy(sig, rfc_sig, 64);
    sig[63] |= 0xe0;
    CHECK(crypto_ed25519_check(sig, rfc_msg, sizeof(rfc_msg), rfc_pk) != 0,
          "non-canonical S (S >= l) is REJECTED");
}

static void test_selftest_gate_passes(void)
{
    /* The same gate the controller invokes at boot must succeed on good code. */
    CHECK(crypto_selftest() == 0, "crypto_selftest() gate PASSES on healthy build");
}

static void test_selftest_gate_fails_fatally(void)
{
    /* Forced-bad vector: the gate must report failure (drives boot panic). */
    uint8_t bad_sig[64];
    memcpy(bad_sig, rfc_sig, 64);
    bad_sig[10] ^= 0xff;  /* corrupt */
    int rc = crypto_selftest_with_vectors(bad_sig, rfc_msg, sizeof(rfc_msg), rfc_pk);
    CHECK(rc != 0, "forced-bad vector makes selftest gate FAIL (would abort boot)");
}

int main(void)
{
    printf("TAP version 13\n");
    printf("# agentOS Ed25519 cryptographic selftest validation (agentos-c7i)\n");

    test_known_good();
    test_known_bad();
    test_selftest_gate_passes();
    test_selftest_gate_fails_fatally();

    printf("1..%d\n", 10);
    if (g_failures) {
        printf("# FAILED: %d cryptographic selftest assertion(s) failed\n", g_failures);
        /* Nonzero exit mirrors the fatal boot gate: a crypto failure aborts. */
        return 1;
    }
    printf("# all cryptographic selftest assertions passed\n");
    return 0;
}
