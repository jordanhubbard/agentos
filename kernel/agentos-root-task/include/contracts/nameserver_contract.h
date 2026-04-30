/*
 * NameServer IPC Contract
 *
 * The NameServer is a passive seL4 protection domain (priority 170) that
 * provides runtime service discovery for all other PDs.  Rather than
 * hard-coding channel constants in every caller, PDs register themselves
 * at boot and peer PDs query by name at runtime.
 *
 * Channel: CH_NAMESERVER (18) — controller PPCs into nameserver.
 *
 * Shared memory:
 *   ns_registry_shmem (8KB) — used by OP_NS_LIST to return entries;
 *   the controller maps this region and reads ns_list_entry_t[] after the call.
 *
 * Invariants:
 *   - All name fields are NUL-padded, not NUL-terminated; consumers must
 *     treat any byte past the first NUL as padding.
 *   - name[NS_NAME_MR_COUNT] occupies MRs 5-8 for REGISTER and MRs 1-4
 *     for LOOKUP/LOOKUP_GATED; each MR carries one seL4_Word of the
 *     32-byte NUL-padded service name.
 *   - OP_NS_LOOKUP_GATED behaves identically to OP_NS_LOOKUP but the
 *     nameserver validates the caller's badge before returning routing info.
 *   - OP_NS_LIST writes up to (8192 / sizeof(ns_list_entry_t)) entries into
 *     ns_registry_shmem before replying with entry_count.
 */

#pragma once
#include <stdint.h>
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define NS_CH_CONTROLLER   CH_NAMESERVER   /* controller → nameserver */

/* ─── Version / name ABI ────────────────────────────────────────────────── */
#ifndef NS_VERSION
#define NS_VERSION          1u
#endif
#ifndef NS_NAME_MAX
#define NS_NAME_MAX         32u
#endif
#ifndef NS_NAME_MR_COUNT
#define NS_NAME_MR_COUNT    4u
#endif

/* ─── Opcodes (placed in MR0) ────────────────────────────────────────────── */
#ifndef OP_NS_REGISTER
#define OP_NS_REGISTER       0xD0u  /* register this PD's service endpoint */
#define OP_NS_LOOKUP         0xD1u  /* look up a service by name */
#define OP_NS_UPDATE_STATUS  0xD2u  /* update liveness/status of own entry */
#define OP_NS_LIST           0xD3u  /* dump all entries into ns_registry_shmem */
#define OP_NS_DEREGISTER     0xD4u  /* remove own service entry */
#define OP_NS_HEALTH         0xD5u  /* return registered_count + NS_VERSION */
#define OP_NS_LOOKUP_GATED   0xD6u  /* look up with caller badge validation */
#endif

/* ─── Service status values ──────────────────────────────────────────────── */
#ifndef NS_STATUS_UNKNOWN
#define NS_STATUS_UNKNOWN   0u
#define NS_STATUS_READY     1u
#define NS_STATUS_DEGRADED  2u
#define NS_STATUS_OFFLINE   3u
#endif

/* ─── Error codes (MR0 in replies) ───────────────────────────────────────── */
#ifndef NS_OK
#define NS_OK               0u
#define NS_ERR_FULL         1u
#define NS_ERR_NOT_FOUND    2u
#define NS_ERR_DUPLICATE    3u
#define NS_ERR_UNKNOWN_OP   4u
#define NS_ERR_BAD_ARGS     5u
#define NS_ERR_FORBIDDEN    6u
#endif

/* ─── Request structs ────────────────────────────────────────────────────── */

/* OP_NS_REGISTER
 * MR0=op, MR1=channel_id, MR2=pd_id, MR3=cap_classes, MR4=version,
 * MR5-8=name[NS_NAME_MR_COUNT]
 */
struct __attribute__((packed)) nameserver_req_register {
    uint32_t op;          /* OP_NS_REGISTER */
    uint32_t channel_id;  /* channel number callers should use to reach this service */
    uint32_t pd_id;       /* numeric PD identifier */
    uint32_t cap_classes; /* capability class bitmask (CAP_CLASS_*) */
    uint32_t version;     /* service interface version */
    uintptr_t name[NS_NAME_MR_COUNT]; /* service name: 32 bytes, NUL-padded */
};

/* OP_NS_LOOKUP / OP_NS_LOOKUP_GATED
 * MR0=op, MR1-4=name[NS_NAME_MR_COUNT]
 */
struct __attribute__((packed)) nameserver_req_lookup {
    uint32_t op;          /* OP_NS_LOOKUP or OP_NS_LOOKUP_GATED */
    uintptr_t name[NS_NAME_MR_COUNT]; /* service name to search for */
};

/* OP_NS_UPDATE_STATUS
 * MR0=op, MR1=channel_id, MR2=new_status
 */
struct __attribute__((packed)) nameserver_req_update_status {
    uint32_t op;          /* OP_NS_UPDATE_STATUS */
    uint32_t channel_id;  /* identifies which entry to update */
    uint32_t new_status;  /* implementation-defined liveness value */
};

/* OP_NS_LIST
 * MR0=op  (no additional MRs; results appear in ns_registry_shmem)
 */
struct __attribute__((packed)) nameserver_req_list {
    uint32_t op;          /* OP_NS_LIST */
};

/* OP_NS_DEREGISTER
 * MR0=op, MR1=channel_id
 */
struct __attribute__((packed)) nameserver_req_deregister {
    uint32_t op;          /* OP_NS_DEREGISTER */
    uint32_t channel_id;  /* channel_id of entry to remove */
};

/* OP_NS_HEALTH
 * MR0=op  (no additional MRs)
 */
struct __attribute__((packed)) nameserver_req_health {
    uint32_t op;          /* OP_NS_HEALTH */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

/* OP_NS_REGISTER reply: MR0=status */
struct __attribute__((packed)) nameserver_reply_register {
    uint32_t status;      /* NS_OK or NS_ERR_* */
};

/* OP_NS_LOOKUP / OP_NS_LOOKUP_GATED reply:
 * MR0=status, MR1=channel_id, MR2=pd_id, MR3=status, MR4=cap_classes, MR5=version
 * Note: MR3 here is the service's own liveness status, not the IPC error code.
 */
struct __attribute__((packed)) nameserver_reply_lookup {
    uint32_t status;      /* NS_OK or NS_ERR_* */
    uint32_t channel_id;
    uint32_t pd_id;
    uint32_t svc_status;  /* service liveness status from last UPDATE_STATUS */
    uint32_t cap_classes;
    uint32_t version;
};

/* OP_NS_UPDATE_STATUS reply: MR0=status */
struct __attribute__((packed)) nameserver_reply_update_status {
    uint32_t status;      /* NS_OK or NS_ERR_* */
};

/* OP_NS_LIST reply: MR0=status, MR1=entry_count */
struct __attribute__((packed)) nameserver_reply_list {
    uint32_t status;       /* NS_OK or NS_ERR_* */
    uint32_t entry_count;  /* number of ns_entry_t records written to shmem */
};

/* OP_NS_DEREGISTER reply: MR0=status */
struct __attribute__((packed)) nameserver_reply_deregister {
    uint32_t status;       /* NS_OK or NS_ERR_* */
};

/* OP_NS_HEALTH reply: MR0=status, MR1=registered_count, MR2=NS_VERSION */
struct __attribute__((packed)) nameserver_reply_health {
    uint32_t status;            /* NS_OK or NS_ERR_* */
    uint32_t registered_count;
    uint32_t ns_version;        /* NS_VERSION constant */
};

/* ─── Shmem layout ──────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    char     name[NS_NAME_MAX];
    uint32_t channel_id;
    uint32_t pd_id;
    uint32_t cap_classes;
    uint32_t version;
    uint8_t  status;
    uint8_t  flags;
    uint8_t  _pad[6];
    uint64_t registered_at;
} nameserver_contract_list_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t count;
    uint8_t  _pad[8];
    nameserver_contract_list_entry_t entries[];
} nameserver_contract_list_header_t;
