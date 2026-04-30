#pragma once
/* contracts/nameserver/interface.h
 * Nameserver contract v1 — capability registry for agentOS.
 *
 * The nameserver is a Ring-2 system service PD that acts as the authoritative
 * registry for all other service endpoints.  At boot each service registers
 * its endpoint capability with OP_NS_REGISTER.  Clients discover services at
 * runtime via OP_NS_LOOKUP, receiving a badged copy of the endpoint capability
 * minted specifically for that client_id.
 *
 * All messages use the sel4_msg_t wire format defined in
 * contracts/sel4-ipc/interface.h.  The ns_request_t / ns_reply_t structs are
 * placed in the sel4_msg_t.data field.
 *
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
 * Opcodes (placed in sel4_msg_t.opcode)
 * --------------------------------------------------------------------------- */
#define OP_NS_REGISTER   0x0001u  /* service registers its endpoint cap */
#define OP_NS_LOOKUP     0x0002u  /* client requests a badged cap copy */
#define OP_NS_REVOKE     0x0003u  /* revoke all minted copies for a service */
#define OP_NS_LIST       0x0004u  /* enumerate registered service names */

/* ---------------------------------------------------------------------------
 * Limits
 * --------------------------------------------------------------------------- */
#define NS_SERVICE_NAME_MAX  48u  /* max service name length including NUL */
#define NS_MAX_SERVICES      64u  /* max simultaneously registered services */

/* ---------------------------------------------------------------------------
 * Request payload (sent as sel4_msg_t.data for all opcodes)
 *
 * For OP_NS_REGISTER: populate both fields.
 * For OP_NS_LOOKUP / OP_NS_REVOKE: populate service_name only; ep_cap_slot
 *   is ignored.
 * For OP_NS_LIST: both fields are ignored; the nameserver replies with
 *   successive OP_NS_LIST replies (one per registered name) terminated by a
 *   reply with an empty service_name.
 * --------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    char     service_name[NS_SERVICE_NAME_MAX]; /* NUL-terminated service name */
    uint64_t ep_cap_slot;  /* OP_NS_REGISTER: root-task-relative cap slot */
} ns_request_t;

_Static_assert(sizeof(ns_request_t) == 56, "ns_request_t must fit in sel4_msg_t.data");

/* ---------------------------------------------------------------------------
 * Reply payload (sent as sel4_msg_t.data in the nameserver's reply)
 *
 * error:       SEL4_ERR_* code; non-zero means failure; remaining fields
 *              are undefined on failure.
 * service_id:  assigned by the nameserver on OP_NS_REGISTER; echoed back
 *              unchanged on OP_NS_LOOKUP so the caller can embed it in the
 *              badge of subsequent calls.
 * ep_cap_slot: OP_NS_LOOKUP only — the CNode-relative slot in the caller's
 *              CSpace into which the nameserver has copied a freshly badged
 *              endpoint capability.  Zero for all other opcodes.
 * --------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t error;        /* SEL4_ERR_* */
    uint16_t service_id;   /* assigned on REGISTER; echoed on LOOKUP */
    uint16_t _pad;         /* reserved, must be zero */
    uint64_t ep_cap_slot;  /* LOOKUP: caller-CNode-relative slot of minted cap */
} ns_reply_t;

_Static_assert(sizeof(ns_reply_t) == 16, "ns_reply_t size check");
