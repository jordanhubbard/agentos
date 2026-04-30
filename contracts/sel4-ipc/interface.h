#pragma once
/* contracts/sel4-ipc/interface.h
 * seL4 IPC wire format for agentOS — all services use this layout.
 * Version: 1
 */

#ifndef __ASSEMBLER__
#ifdef __has_include
#  if __has_include(<stdint.h>)
#    include <stdint.h>
#  else
     typedef unsigned char      uint8_t;
     typedef unsigned short     uint16_t;
     typedef unsigned int       uint32_t;
     typedef unsigned long long uint64_t;
#  endif
#else
#  include <stdint.h>
#endif
#endif /* __ASSEMBLER__ */

/* ---------------------------------------------------------------------------
 * Badge encoding
 *
 * A seL4 badge is a single seL4_Word (64-bit on 64-bit targets).  agentOS
 * packs three fields into that word:
 *
 *   [63:48]  service_id  — identifies which PD/service owns this endpoint
 *   [47:32]  client_id   — identifies which client PD holds this cap copy
 *   [31:0]   op_token    — per-call nonce / operation context
 * --------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint16_t service_id;  /* [63:48] — which service is being called */
    uint16_t client_id;   /* [47:32] — which PD holds this cap copy */
    uint32_t op_token;    /* [31:0]  — operation context / nonce */
} sel4_badge_t;

_Static_assert(sizeof(sel4_badge_t) == 8, "sel4_badge_t must be 8 bytes");

/* ---------------------------------------------------------------------------
 * Standard message layout
 *
 * seL4 provides 120 bytes of MR space (10 x seL4_Word on a 64-bit kernel).
 * agentOS reserves the first two MRs for the header and caps up to 6 MRs
 * (48 bytes) for payload, keeping the total at 56 bytes so every message
 * fits inside a single seL4 IPC transfer with room for capability transfer
 * metadata.
 * --------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;     /* mr[0] low word  — operation selector */
    uint32_t length;     /* mr[0] high word — payload bytes following (0 if none) */
    uint8_t  data[48];   /* mr[1..6]        — up to 6 MRs of 8 bytes each */
} sel4_msg_t;

_Static_assert(sizeof(sel4_msg_t) == 56, "sel4_msg_t must fit in seL4 IPC buffer");

/* ---------------------------------------------------------------------------
 * Standard error codes
 *
 * Returned in the opcode field of a reply message.  All non-zero values
 * indicate failure.  Service-specific errors start at 0x0100u.
 * --------------------------------------------------------------------------- */
#define SEL4_ERR_OK              0u   /* success */
#define SEL4_ERR_INVALID_OP      1u   /* unknown or unsupported opcode */
#define SEL4_ERR_PERM_DENIED     2u   /* capability check failed */
#define SEL4_ERR_NOT_FOUND       3u   /* requested object does not exist */
#define SEL4_ERR_NO_MEM          4u   /* kernel or service out of memory */
#define SEL4_ERR_BUSY            5u   /* service temporarily unavailable */
#define SEL4_ERR_INVALID_ARG     6u   /* malformed request field */
#define SEL4_ERR_IO              7u   /* underlying I/O failure */

/* ---------------------------------------------------------------------------
 * Badge encode / decode helpers (C only, not available in assembler)
 * --------------------------------------------------------------------------- */
#ifndef __ASSEMBLER__

static inline uint64_t sel4_badge_encode(sel4_badge_t b) {
    return ((uint64_t)b.service_id << 48)
         | ((uint64_t)b.client_id  << 32)
         | ((uint64_t)b.op_token);
}

static inline sel4_badge_t sel4_badge_decode(uint64_t raw) {
    sel4_badge_t b;
    b.service_id = (uint16_t)(raw >> 48);
    b.client_id  = (uint16_t)(raw >> 32);
    b.op_token   = (uint32_t)(raw);
    return b;
}

#endif /* __ASSEMBLER__ */
