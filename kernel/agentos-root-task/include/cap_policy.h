/* cap_policy.h — Runtime capability policy blob format
 *
 * The policy blob is a simple packed array of grant records.
 * At boot, monitor reads from cap_policy_shmem_vaddr.
 * init_agent can reload via OP_CAP_POLICY_RELOAD.
 */

#pragma once

#include <stdint.h>

#define CAP_POLICY_MAGIC    0x43415050u  /* "CAPP" */
#define CAP_POLICY_VERSION  1u
#define CAP_POLICY_MAX_GRANTS 128u

typedef struct __attribute__((packed)) {
    uint32_t magic;          /* CAP_POLICY_MAGIC */
    uint32_t version;        /* CAP_POLICY_VERSION */
    uint32_t num_grants;     /* number of cap_grant_t entries following */
    uint32_t _reserved;
} cap_policy_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  agent_id;       /* PD slot index (0=controller, 1=event_bus, ...) */
    uint8_t  cap_class;      /* capability class bitmask (from AGENTOS_CAP_* defines) */
    uint8_t  rights;         /* r=1, w=2, x=4, grant=8, revoke=16 */
    uint8_t  flags;          /* 0x01 = delegatable, 0x02 = revocable */
    uint32_t resource_id;    /* which resource (0 = default for class) */
} cap_grant_t;
