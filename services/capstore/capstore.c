/*
 * agentOS CapStore — Capability Database Service
 *
 * Tracks all capability derivations, grants, and revocations.
 * Provides audit trail and query interface for capability management.
 * Built on seL4's Capability Derivation Tree (CDT).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Maximum tracked capabilities */
#define CAPSTORE_MAX_ENTRIES 4096

/* Capability types (agentOS semantic layer) */
typedef enum {
    CAP_TYPE_TOOL       = 0x01,
    CAP_TYPE_MODEL      = 0x02,
    CAP_TYPE_MEMORY     = 0x03,
    CAP_TYPE_MSG        = 0x04,
    CAP_TYPE_STORE      = 0x05,
    CAP_TYPE_SPAWN      = 0x06,
    CAP_TYPE_NET        = 0x07,
    CAP_TYPE_SELF       = 0x08,
    CAP_TYPE_SERVICE    = 0x09,
} cap_type_t;

/* Capability entry */
typedef struct {
    uint64_t    cap_id;         /* Unique capability ID */
    uint64_t    parent_id;      /* Parent capability (0 = root) */
    uint8_t     owner[32];      /* AgentID of current holder */
    uint8_t     creator[32];    /* AgentID that created this cap */
    cap_type_t  type;           /* Semantic capability type */
    uint32_t    rights;         /* Rights bitmask */
    uint64_t    created_at;     /* Timestamp */
    uint32_t    flags;          /* Status flags */
    char        label[64];      /* Human/agent-readable label */
} capstore_entry_t;

/* Internal state */
static capstore_entry_t entries[CAPSTORE_MAX_ENTRIES];
static int entry_count = 0;
static uint64_t next_cap_id = 1;

int capstore_init(void) {
    memset(entries, 0, sizeof(entries));
    entry_count = 0;
    next_cap_id = 1;
    printf("[capstore] CapStore initialized (max %d entries)\n", CAPSTORE_MAX_ENTRIES);
    return 0;
}

int capstore_register(uint8_t *owner, cap_type_t type, uint32_t rights,
                       const char *label, uint64_t *out_id) {
    if (entry_count >= CAPSTORE_MAX_ENTRIES) return -1;
    
    int idx = entry_count++;
    entries[idx].cap_id = next_cap_id++;
    entries[idx].parent_id = 0;
    memcpy(entries[idx].owner, owner, 32);
    memcpy(entries[idx].creator, owner, 32);
    entries[idx].type = type;
    entries[idx].rights = rights;
    entries[idx].flags = 0;
    if (label) strncpy(entries[idx].label, label, 63);
    
    if (out_id) *out_id = entries[idx].cap_id;
    
    printf("[capstore] Registered cap %llu: type=%d rights=0x%x label='%s'\n",
           (unsigned long long)entries[idx].cap_id, type, rights, 
           label ? label : "");
    return 0;
}

int capstore_derive(uint64_t parent_id, uint8_t *new_owner, uint32_t rights,
                     uint64_t *out_id) {
    /* Find parent */
    int parent_idx = -1;
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].cap_id == parent_id) {
            parent_idx = i;
            break;
        }
    }
    if (parent_idx < 0) return -1;
    
    /* Cannot escalate: new rights must be subset of parent */
    if ((rights & entries[parent_idx].rights) != rights) return -2;
    
    if (entry_count >= CAPSTORE_MAX_ENTRIES) return -3;
    
    int idx = entry_count++;
    entries[idx].cap_id = next_cap_id++;
    entries[idx].parent_id = parent_id;
    memcpy(entries[idx].owner, new_owner, 32);
    memcpy(entries[idx].creator, entries[parent_idx].owner, 32);
    entries[idx].type = entries[parent_idx].type;
    entries[idx].rights = rights;
    entries[idx].flags = 0;
    snprintf(entries[idx].label, 64, "derived from %llu",
             (unsigned long long)parent_id);
    
    if (out_id) *out_id = entries[idx].cap_id;
    
    printf("[capstore] Derived cap %llu from %llu (rights=0x%x)\n",
           (unsigned long long)entries[idx].cap_id,
           (unsigned long long)parent_id, rights);
    return 0;
}

int capstore_revoke(uint64_t cap_id) {
    /* Revoke a capability and ALL its descendants */
    int revoked = 0;
    
    /* Mark the target */
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].cap_id == cap_id) {
            entries[i].flags |= 0x01; /* REVOKED */
            revoked++;
            break;
        }
    }
    
    /* Cascade: revoke all children */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < entry_count; i++) {
            if (entries[i].flags & 0x01) continue;  /* Already revoked */
            
            /* Check if parent is revoked */
            for (int j = 0; j < entry_count; j++) {
                if (entries[j].cap_id == entries[i].parent_id &&
                    (entries[j].flags & 0x01)) {
                    entries[i].flags |= 0x01;
                    revoked++;
                    changed = 1;
                    break;
                }
            }
        }
    }
    
    printf("[capstore] Revoked cap %llu and %d descendants\n",
           (unsigned long long)cap_id, revoked - 1);
    return 0;
}

int capstore_query_by_owner(uint8_t *owner, uint64_t **out_ids, int *out_count) {
    static uint64_t result_buf[CAPSTORE_MAX_ENTRIES];
    int count = 0;
    
    for (int i = 0; i < entry_count; i++) {
        if (!(entries[i].flags & 0x01) &&  /* Not revoked */
            memcmp(entries[i].owner, owner, 32) == 0) {
            result_buf[count++] = entries[i].cap_id;
        }
    }
    
    *out_ids = result_buf;
    *out_count = count;
    return 0;
}
