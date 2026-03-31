/*
 * agentOS Capability Broker
 *
 * The capability broker runs inside the monitor PD (compiled into monitor.elf).
 * It manages:
 *   - Capability grant requests from agents
 *   - Capability revocation
 *   - The capability audit log
 *
 * In seL4, capability operations are kernel operations.
 * The broker's job is the policy: who gets what, and when.
 *
 * v0.1: Static capability table. Dynamic delegation in v0.2.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

#define MAX_CAPS 256

/* Simple capability registry */
typedef struct {
    bool active;
    agentos_cap_desc_t cap;
    uint32_t owner_pd;    /* PD that owns this capability */
    uint32_t granted_to;  /* PD it was granted to (0 = not granted) */
    bool     revokable;   /* Can the owner revoke this? */
    uint64_t grant_time;  /* When was it granted (boot sequence) */
} cap_entry_t;

static cap_entry_t cap_table[MAX_CAPS];
static uint32_t cap_count = 0;
static uint64_t audit_seq = 0;

/* Initialize the capability broker */
void cap_broker_init(void) {
    microkit_dbg_puts("[cap_broker] Initializing capability table\n");
    
    for (int i = 0; i < MAX_CAPS; i++) {
        cap_table[i].active = false;
    }
    
    microkit_dbg_puts("[cap_broker] Ready\n");
}

/* Register a capability in the table */
int cap_broker_register(uint32_t owner_pd, agentos_cap_desc_t cap, bool revokable) {
    for (int i = 0; i < MAX_CAPS; i++) {
        if (!cap_table[i].active) {
            cap_table[i].active     = true;
            cap_table[i].cap        = cap;
            cap_table[i].owner_pd   = owner_pd;
            cap_table[i].granted_to = 0;
            cap_table[i].revokable  = revokable;
            cap_table[i].grant_time = 0;
            cap_count++;
            return i; /* return handle */
        }
    }
    return -1; /* table full */
}

/* Grant a capability to another PD */
bool cap_broker_grant(int handle, uint32_t to_pd, uint64_t boot_seq) {
    if (handle < 0 || handle >= MAX_CAPS || !cap_table[handle].active) {
        return false;
    }
    
    cap_entry_t *entry = &cap_table[handle];
    
    /* Can't grant a capability that's already granted elsewhere */
    if (entry->granted_to != 0 && entry->granted_to != to_pd) {
        microkit_dbg_puts("[cap_broker] DENY: capability already granted to another PD\n");
        return false;
    }
    
    entry->granted_to = to_pd;
    entry->grant_time = boot_seq;
    audit_seq++;
    
    microkit_dbg_puts("[cap_broker] Capability granted\n");
    return true;
}

/* Revoke a granted capability */
bool cap_broker_revoke(int handle, uint32_t requesting_pd) {
    if (handle < 0 || handle >= MAX_CAPS || !cap_table[handle].active) {
        return false;
    }
    
    cap_entry_t *entry = &cap_table[handle];
    
    /* Only the owner can revoke */
    if (entry->owner_pd != requesting_pd) {
        microkit_dbg_puts("[cap_broker] DENY: revocation by non-owner\n");
        return false;
    }
    
    if (!entry->revokable) {
        microkit_dbg_puts("[cap_broker] DENY: capability is not revokable\n");
        return false;
    }
    
    entry->granted_to = 0;
    audit_seq++;
    
    microkit_dbg_puts("[cap_broker] Capability revoked\n");
    return true;
}

/* Check if a PD has access to a capability */
bool cap_broker_check(uint32_t pd, uint32_t cptr, uint32_t required_rights) {
    for (int i = 0; i < MAX_CAPS; i++) {
        if (!cap_table[i].active) continue;
        if (cap_table[i].cap.cptr != cptr) continue;
        
        bool is_owner   = (cap_table[i].owner_pd == pd);
        bool is_grantee = (cap_table[i].granted_to == pd);
        
        if (is_owner || is_grantee) {
            /* Check rights */
            if ((cap_table[i].cap.rights & required_rights) == required_rights) {
                return true;
            }
        }
    }
    return false;
}

/* ── Capability Attestation (OP_CAP_ATTEST) ─────────────────────────────── */
/*
 * cap_broker_attest() — serialize the active capability table into a compact
 * text record suitable for signing and audit.
 *
 * Output format (written to attestation_buf, null-terminated):
 *   ATTEST\t<seq>\t<timestamp>\n
 *   CAP\t<handle>\t<owner_pd>\t<granted_to>\t<cptr_hex>\t<rights_hex>\t<kind_hex>\t<badge_hex>\t<revokable>\t<grant_time>\n
 *   ...
 *   END\t<num_caps>\n
 *
 * The caller (monitor PD) signs this buffer with the system Ed25519 key and
 * writes it to AgentFS as agentos/attestation/<seq>.cbor (or .txt for v1).
 *
 * Returns the number of bytes written (0 on buffer overflow).
 */
uint32_t cap_broker_attest(char *buf, uint32_t buf_len, uint64_t timestamp) {
    if (!buf || buf_len < 64) return 0;
    uint32_t pos = 0;
    uint32_t active = 0;

#define ATTEST_APPEND(fmt, ...) do { \
    int n = snprintf(buf + pos, buf_len - pos, fmt, ##__VA_ARGS__); \
    if (n < 0 || (uint32_t)n >= buf_len - pos) return 0; \
    pos += (uint32_t)n; \
} while(0)

    audit_seq++;
    ATTEST_APPEND("ATTEST\t%llu\t%llu\n",
                  (unsigned long long)audit_seq,
                  (unsigned long long)timestamp);

    for (int i = 0; i < MAX_CAPS; i++) {
        const cap_entry_t *e = &cap_table[i];
        if (!e->active) continue;
        active++;
        ATTEST_APPEND("CAP\t%d\t%u\t%u\t0x%x\t0x%x\t0x%x\t0x%x\t%d\t%llu\n",
                      i,
                      e->owner_pd,
                      e->granted_to,
                      e->cap.cptr,
                      e->cap.rights,
                      e->cap.kind,
                      e->cap.badge,
                      e->revokable ? 1 : 0,
                      (unsigned long long)e->grant_time);
    }
    ATTEST_APPEND("END\t%u\n", active);

#undef ATTEST_APPEND
    return pos;
}
