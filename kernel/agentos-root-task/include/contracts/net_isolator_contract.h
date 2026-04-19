#pragma once
/* NET_ISOLATOR contract — version 1
 * PD: net_isolator | Source: src/net_isolator.c | Channel: CH_CONTROLLER_NET_ISOLATOR=61 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define NET_ISOLATOR_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_NET_ISOLATOR            61   /* controller -> net_isolator; cross-ref: channels_generated.h */

/* ── Opcodes ── */
#define NET_ISOLATOR_OP_ALLOW      0xA900u  /* add an allow rule to the ACL */
#define NET_ISOLATOR_OP_DENY       0xA901u  /* add a deny rule to the ACL */
#define NET_ISOLATOR_OP_STATUS     0xA902u  /* query ACL size and packet statistics */
#define NET_ISOLATOR_OP_ACL_DUMP   0xA903u  /* dump ACL entries to shared memory */
#define NET_ISOLATOR_OP_FLUSH      0xA904u  /* remove all rules for a given agent/slot */

/* ── ACL rule flags ── */
#define NET_ACL_FLAG_INGRESS       (1u << 0)  /* rule applies to inbound traffic */
#define NET_ACL_FLAG_EGRESS        (1u << 1)  /* rule applies to outbound traffic */
#define NET_ACL_FLAG_TCP           (1u << 2)  /* TCP protocol */
#define NET_ACL_FLAG_UDP           (1u << 3)  /* UDP protocol */
#define NET_ACL_FLAG_ICMP          (1u << 4)  /* ICMP/ICMPv6 */
#define NET_ACL_FLAG_LOG           (1u << 5)  /* log matches */
#define NET_ACL_FLAG_WILDCARD_PORT (1u << 6)  /* match any port */
#define NET_ACL_FLAG_WILDCARD_ADDR (1u << 7)  /* match any remote address */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* NET_ISOLATOR_OP_ALLOW or NET_ISOLATOR_OP_DENY */
    uint32_t slot_id;         /* worker slot this rule applies to */
    uint8_t  remote_addr[16]; /* remote IPv6/IPv4-mapped address */
    uint16_t remote_port;     /* remote port (0 if WILDCARD_PORT) */
    uint16_t local_port;      /* local port bound to this slot (0 = any) */
    uint32_t flags;           /* NET_ACL_FLAG_* */
    uint32_t priority;        /* rule priority (higher = evaluated first) */
} net_isolator_req_rule_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else net_isolator_error_t */
    uint32_t rule_id;         /* opaque handle for rule removal */
} net_isolator_reply_rule_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* NET_ISOLATOR_OP_STATUS */
} net_isolator_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t rule_count;      /* total ACL rules installed */
    uint64_t packets_allowed; /* total packets allowed since boot */
    uint64_t packets_denied;  /* total packets denied since boot */
    uint32_t slots_tracked;   /* number of slots with at least one rule */
} net_isolator_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* NET_ISOLATOR_OP_ACL_DUMP */
    uint32_t slot_id;         /* 0xFFFFFFFF = dump all rules */
    uint32_t shmem_offset;
    uint32_t max_entries;
} net_isolator_req_acl_dump_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t entry_count;
} net_isolator_reply_acl_dump_t;

/* ACL entry written to shmem during dump */
typedef struct __attribute__((packed)) {
    uint32_t rule_id;
    uint32_t slot_id;
    uint8_t  remote_addr[16];
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t flags;
    uint32_t priority;
    uint32_t action;          /* 0 = allow, 1 = deny */
    uint64_t match_count;     /* packets matched by this rule */
} net_isolator_acl_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* NET_ISOLATOR_OP_FLUSH */
    uint32_t slot_id;         /* 0xFFFFFFFF = flush all rules */
} net_isolator_req_flush_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t rules_removed;
} net_isolator_reply_flush_t;

/* ── Error codes ── */
typedef enum {
    NET_ISOLATOR_OK           = 0,
    NET_ISOLATOR_ERR_FULL     = 1,  /* ACL table full */
    NET_ISOLATOR_ERR_BAD_SLOT = 2,  /* slot_id not valid */
    NET_ISOLATOR_ERR_NO_RULE  = 3,  /* rule_id not found */
    NET_ISOLATOR_ERR_BAD_ADDR = 4,  /* malformed remote_addr */
    NET_ISOLATOR_ERR_NO_CAP   = 5,  /* caller lacks AGENTOS_CAP_NET */
} net_isolator_error_t;

/* ── Invariants ──
 * - Default policy is DENY for all slots with at least one rule installed.
 * - Slots with no rules are pass-through (all traffic allowed) — install a default deny rule.
 * - Rules are evaluated in priority order (highest first); first match wins.
 * - FLUSH removes all rules for a slot; reverts it to pass-through behavior.
 * - Packet statistics are per-direction and per-rule; aggregate is in STATUS reply.
 * - Only the controller (Ring 1) may modify ACL rules; agents cannot call this PD directly.
 */
