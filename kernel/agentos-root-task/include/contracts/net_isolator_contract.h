/*
 * NetIsolator IPC Contract
 *
 * The NetIsolator PD enforces per-agent network access control lists (ACLs).
 * Callers add or remove per-PD allow/deny rules; NetIsolator consults the
 * ACL on each packet from the net_pd layer.
 *
 * Channel: CH_NET_ISOLATOR (see agentos.h)
 * Opcodes: MSG_NET_ALLOW, MSG_NET_DENY, MSG_NET_ISO_STATUS, MSG_NET_ACL_DUMP
 *
 * Invariants:
 *   - Rules are evaluated in insertion order; first match wins.
 *   - Default policy (no rules) is DENY ALL.
 *   - MSG_NET_ALLOW and MSG_NET_DENY are idempotent for identical tuples.
 *   - MSG_NET_ISO_STATUS and MSG_NET_ACL_DUMP are read-only.
 *   - protocol 0xFF means "any protocol".
 *   - port 0 means "any port".
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define NET_ISOLATOR_CH_CONTROLLER  CH_NET_ISOLATOR

/* ─── Request structs ────────────────────────────────────────────────────── */

struct net_iso_req_allow {
    uint32_t pd_id;             /* PD this rule applies to */
    uint8_t  proto;             /* IP protocol number (0xFF = any) */
    uint8_t  direction;         /* NET_ISO_DIR_* */
    uint16_t port;              /* destination port (0 = any) */
    uint32_t remote_ip;         /* IPv4 address (0 = any) */
};

#define NET_ISO_DIR_IN    0
#define NET_ISO_DIR_OUT   1
#define NET_ISO_DIR_BOTH  2

struct net_iso_req_deny {
    uint32_t pd_id;
    uint8_t  proto;
    uint8_t  direction;
    uint16_t port;
    uint32_t remote_ip;
};

struct net_iso_req_status {
    uint32_t pd_id;
};

struct net_iso_req_acl_dump {
    uint32_t pd_id;
    uint32_t max_entries;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct net_iso_reply_allow {
    uint32_t ok;
    uint32_t rule_id;
};

struct net_iso_reply_deny {
    uint32_t ok;
    uint32_t rule_id;
};

struct net_iso_reply_status {
    uint32_t ok;
    uint32_t allow_rules;
    uint32_t deny_rules;
    uint64_t packets_allowed;
    uint64_t packets_denied;
};

struct net_iso_reply_acl_dump {
    uint32_t ok;
    uint32_t count;             /* entries written to shmem */
};

/* ─── Shmem layout: ACL entry ────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t pd_id;
    uint32_t rule_id;
    uint8_t  action;            /* 0=allow 1=deny */
    uint8_t  proto;
    uint8_t  direction;
    uint8_t  _pad;
    uint16_t port;
    uint16_t _pad2;
    uint32_t remote_ip;
} net_iso_acl_entry_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum net_isolator_error {
    NET_ISO_OK              = 0,
    NET_ISO_ERR_TABLE_FULL  = 1,
    NET_ISO_ERR_BAD_PD      = 2,
    NET_ISO_ERR_NOT_FOUND   = 3,
};
