/*
 * CC operator credential.
 *
 * Supplied by the build. NOT a secret from the operator: the platform threat
 * model treats the local operator as untrusted and assumes they can read the
 * image. The credential selects an authority envelope and distinguishes an
 * operator session from an unrelated local process. It does not, and must not
 * be described as, authenticating the operator.
 *
 * AGENTOS_CC_OPERATOR_TOKEN is a 32-byte initializer supplied at build time.
 * When it is not defined the build uses a well-known development value, and
 * that fact is logged at boot so a development image is never mistaken for a
 * provisioned one.
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CC_OPERATOR_TOKEN_BYTES 32u

#ifdef AGENTOS_CC_OPERATOR_TOKEN
#define CC_OPERATOR_TOKEN_IS_DEVELOPMENT 0
static const uint8_t cc_operator_token[CC_OPERATOR_TOKEN_BYTES] = AGENTOS_CC_OPERATOR_TOKEN;
#else
#define CC_OPERATOR_TOKEN_IS_DEVELOPMENT 1
static const uint8_t cc_operator_token[CC_OPERATOR_TOKEN_BYTES] = {
    0x61, 0x67, 0x65, 0x6e, 0x74, 0x4f, 0x53, 0x2d,
    0x64, 0x65, 0x76, 0x2d, 0x6f, 0x70, 0x65, 0x72,
    0x61, 0x74, 0x6f, 0x72, 0x2d, 0x74, 0x6f, 0x6b,
    0x65, 0x6e, 0x2d, 0x76, 0x31, 0x00, 0x00, 0x00,
};
#endif

/* Constant-time over the full length. The credential is not secret today,
 * but this path takes a hardware-backed value later and must not leak
 * position of first difference by timing. */
static inline bool cc_credential_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0u;
    for (uint32_t i = 0u; i < CC_OPERATOR_TOKEN_BYTES; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0u;
}
