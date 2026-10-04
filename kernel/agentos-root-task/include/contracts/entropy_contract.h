/*
 * Entropy service IPC contract
 *
 * entropy_pd is a DRIVER protection domain: it owns one virtio-rng device and
 * nothing else. It is not a virtualizer, because there is no multiplexing
 * decision to make -- every client receives independent output, and no client's
 * request affects another's.
 *
 * SCOPE OF WHAT THIS PROVIDES. The service returns bytes produced by the
 * virtio-rng device it owns. It makes NO claim about statistical quality,
 * entropy estimation, or cryptographic suitability of that source: under QEMU
 * the backing source is the host's RNG, which establishes nothing about a real
 * board. Callers needing a qualified source must state that requirement
 * separately; this contract only promises that bytes came from the device.
 *
 * Opcode: MSG_ENTROPY_GET. Request and reply are the packed structs below;
 * the reply payload travels in the message registers.
 */

#pragma once
#include <stdint.h>

#define AOS_ENTROPY_VERSION    1u
#define AOS_ENTROPY_MAX_BYTES  64u

#ifndef MSG_ENTROPY_GET
#define MSG_ENTROPY_GET        0x2700u
#endif

#define AOS_ENTROPY_OK             0
#define AOS_ENTROPY_ERR_VERSION    1
#define AOS_ENTROPY_ERR_RANGE      2
#define AOS_ENTROPY_ERR_UNAVAILABLE 3

typedef struct __attribute__((packed)) aos_entropy_req {
    uint32_t version;
    uint32_t length;    /* 1..AOS_ENTROPY_MAX_BYTES */
    uint32_t reserved;  /* must be zero */
} aos_entropy_req_t;

typedef struct __attribute__((packed)) aos_entropy_reply {
    uint32_t status;
    uint32_t length;    /* bytes valid in data[] */
    uint8_t  data[AOS_ENTROPY_MAX_BYTES];
} aos_entropy_reply_t;

int aos_entropy_validate_req(const aos_entropy_req_t *req);
