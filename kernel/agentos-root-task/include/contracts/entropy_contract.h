/*
 * Entropy service IPC contract
 *
 * THE ONLY IMPLEMENTATION TODAY (entropy_pd on QEMU virt) RETURNS
 * AOS_ENTROPY_ERR_UNAVAILABLE TO EVERY REQUEST. QEMU's virt machine
 * hard-caps virtio-mmio at 32 slots, and all 32 are already exclusively
 * owned by other driver PDs before entropy_pd exists, so no virtio-rng
 * device can be wired to it on this machine -- see
 * services/entropy-service/entropy_svc.c and docs/TCB.md for the full
 * account. entropy_pd still answers this opcode (reachable, validates
 * requests, never hangs); it just has nothing to read from.
 *
 * entropy_pd is a DRIVER protection domain: it owns one virtio-rng device
 * when one exists, and nothing else. It is not a virtualizer, because
 * there is no multiplexing decision to make -- every client receives
 * independent output, and no client's request affects another's.
 *
 * SCOPE OF WHAT THIS PROVIDES WHEN A DEVICE EXISTS. This is a statement
 * about a future live source, not about current behaviour on this
 * branch -- see above. When entropy_pd does own a device, the service
 * returns bytes produced by it and makes NO claim about statistical
 * quality, entropy estimation, or cryptographic suitability of that
 * source: under QEMU the backing source would be the host's RNG, which
 * establishes nothing about a real board. Callers needing a qualified
 * source must state that requirement separately; this contract only ever
 * promises that bytes came from the device.
 *
 * Opcode: MSG_ENTROPY_GET. Request and reply are the packed structs below;
 * the reply payload travels in the message registers.
 */

#pragma once
#include <stdint.h>

#define AOS_ENTROPY_VERSION    1u

/*
 * AOS_ENTROPY_MAX_BYTES is wire-derived, not an arbitrary policy choice:
 * aos_entropy_reply_t travels inline in a sel4_msg_t, which carries
 * SEL4_MSG_DATA_BYTES (48) bytes total. entropy_pd's reply spends the
 * first 8 of those on a status word and a length word, leaving 40 for
 * data; 32 is the largest round number that fits with margin, and it is
 * also a complete 256-bit key or nonce -- the actual use. A caller needing
 * more than 32 bytes in one call needs a shmem-backed opcode, not a
 * larger value here.
 */
#define AOS_ENTROPY_MAX_BYTES  32u

#ifndef MSG_ENTROPY_GET
#define MSG_ENTROPY_GET        0x2C01u
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
