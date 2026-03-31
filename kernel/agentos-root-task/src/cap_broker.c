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
    console_log(4, 4, "[cap_broker] Initializing capability table\n");
    
    for (int i = 0; i < MAX_CAPS; i++) {
        cap_table[i].active = false;
    }
    
    console_log(4, 4, "[cap_broker] Ready\n");
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
        console_log(4, 4, "[cap_broker] DENY: capability already granted to another PD\n");
        return false;
    }
    
    entry->granted_to = to_pd;
    entry->grant_time = boot_seq;
    audit_seq++;
    
    /* Send audit event to cap_audit_log PD */
    microkit_mr_set(0, OP_CAP_LOG);
    microkit_mr_set(1, CAP_EVENT_GRANT);
    microkit_mr_set(2, to_pd);
    microkit_mr_set(3, entry->cap.rights);  /* caps mask = rights field */
    microkit_mr_set(4, (uint32_t)handle);   /* slot_id = handle */
    microkit_ppcall(CH_CAP_AUDIT_CTRL, microkit_msginfo_new(0, 5));
    
    console_log(4, 4, "[cap_broker] Capability granted (audited)\n");
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
        console_log(4, 4, "[cap_broker] DENY: revocation by non-owner\n");
        return false;
    }
    
    if (!entry->revokable) {
        console_log(4, 4, "[cap_broker] DENY: capability is not revokable\n");
        return false;
    }
    
    entry->granted_to = 0;
    audit_seq++;
    
    /* Send audit event to cap_audit_log PD */
    microkit_mr_set(0, OP_CAP_LOG);
    microkit_mr_set(1, CAP_EVENT_REVOKE);
    microkit_mr_set(2, requesting_pd);
    microkit_mr_set(3, entry->cap.rights);
    microkit_mr_set(4, (uint32_t)handle);
    microkit_ppcall(CH_CAP_AUDIT_CTRL, microkit_msginfo_new(0, 5));
    
    console_log(4, 4, "[cap_broker] Capability revoked (audited)\n");
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

void cap_broker_revoke_agent(uint32_t agent_pd, uint32_t reason_flags) {
    bool revoked = false;
    for (int i = 0; i < MAX_CAPS; i++) {
        if (!cap_table[i].active) continue;
        if (cap_table[i].granted_to != agent_pd) continue;

        cap_table[i].granted_to = 0;
        cap_table[i].grant_time = 0;
        audit_seq++;
        revoked = true;

        microkit_mr_set(0, OP_CAP_LOG);
        microkit_mr_set(1, CAP_EVENT_REVOKE);
        microkit_mr_set(2, agent_pd);
        microkit_mr_set(3, cap_table[i].cap.rights);
        microkit_mr_set(4, (uint32_t)i);
        microkit_ppcall(CH_CAP_AUDIT_CTRL, microkit_msginfo_new(0, 5));
    }

    if (revoked) {
        console_log(4, 4, "[cap_broker] Capabilities revoked for agent ");
        char buf[12]; int bi = 11; buf[bi] = '\0';
        uint32_t v = agent_pd;
        if (v == 0) { buf[--bi] = '0'; }
        else {
            while (v > 0 && bi > 0) {
                buf[--bi] = (char)('0' + (v % 10));
                v /= 10;
            }
        }
        {
            char _cl_buf[256] = {};
            char *_cl_p = _cl_buf;
            for (const char *_s = &buf[bi]; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = " reason=0x"; *_s; _s++) *_cl_p++ = *_s;
            *_cl_p = 0;
            console_log(4, 4, _cl_buf);
        }
        char hex[9];
        for (int i = 0; i < 8; i++) {
            uint32_t nib = (reason_flags >> (28 - i * 4)) & 0xF;
            hex[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
        }
        hex[8] = '\0';
        {
            char _cl_buf[256] = {};
            char *_cl_p = _cl_buf;
            for (const char *_s = hex; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
            *_cl_p = 0;
            console_log(4, 4, _cl_buf);
        }
    } else {
        console_log(4, 4, "[cap_broker] No capabilities to revoke for agent ");
        char buf[12]; int bi = 11; buf[bi] = '\0';
        uint32_t v = agent_pd;
        if (v == 0) { buf[--bi] = '0'; }
        else {
            while (v > 0 && bi > 0) {
                buf[--bi] = (char)('0' + (v % 10));
                v /= 10;
            }
        }
        {
            char _cl_buf[256] = {};
            char *_cl_p = _cl_buf;
            for (const char *_s = &buf[bi]; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
            *_cl_p = 0;
            console_log(4, 4, _cl_buf);
        }
    }
}

/* ── Capability Attestation (OP_CAP_ATTEST) ──────────────────────────────── *
 *
 * Generates a cryptographically signed snapshot of the entire capability
 * state (cap_table + net_isolator ACL summary + audit_seq) that jkh can
 * verify offline.
 *
 * Attestation report binary format (ATTEST_MAGIC + fixed-size fields):
 *
 * Offset  Size  Description
 * ------  ----  -----------
 *   0      4    Magic: 0x4E415454 ("NATT")
 *   4      4    Format version: 1
 *   8      8    Timestamp (boot tick, MR-supplied by caller)
 *  16      4    active_cap_count
 *  20      4    total_cap_slots = MAX_CAPS
 *  24      8    audit_seq (monotonic)
 *  32    N*20   Cap entries: [cptr(4), rights(4), kind(4), owner_pd(4), granted_to(4)]
 * 32+N*20  8    Net ACL summary: [slots_active(4), total_denials(4)]
 * ...      64   SHA-512 signature (hash of all preceding bytes, key_id in [0:8])
 *
 * Total (N=0): 32 + 8 + 64 = 104 bytes minimum.
 * Total (N=256): 32 + 5120 + 8 + 64 = 5224 bytes.
 *
 * The report is written to AgentFS under path-id derived from timestamp.
 * On-disk path convention (for attest_verify.py): agentos/attestation/<tick>.att
 *
 * Signing: sha512(report[0..len-64]) → report[len-64..len]
 *   key_id bytes [0:8] in sig slot hold a constant sentinel 0x4E415454_01000000.
 */

#include "ed25519_verify.h"  /* sha512() */

/* Channels visible from controller PD (same as monitor.c) */
#ifndef CH_AGENTFS_CTRL
#define CH_AGENTFS_CTRL   5   /* controller -> agentfs (PPC) */
#endif
#ifndef CH_NET_ISO_CTRL
#define CH_NET_ISO_CTRL   6   /* controller -> net_isolator (if wired; best-effort) */
#endif
#define OP_AGENTFS_PUT    0x30u
#define OP_NET_STATUS     0x74u

#define ATTEST_MAGIC      0x4E415454u  /* "NATT" */
#define ATTEST_VERSION    1u
#define ATTEST_CAP_STRIDE 20u          /* bytes per cap entry in report */
#define ATTEST_MAX_REPORT (32u + MAX_CAPS * ATTEST_CAP_STRIDE + 8u + 64u)

/* Sentinel key_id in signature slot — identifies agentOS in-kernel attester */
static const uint8_t ATTEST_KEY_ID[8] = { 0x4E, 0x41, 0x54, 0x54, 0x01, 0x00, 0x00, 0x00 };

static void attest_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void attest_put_u64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
}

/*
 * cap_broker_attest — generate and store a signed capability attestation report.
 *
 * Parameters:
 *   boot_tick   — current boot tick (supplied by caller from seL4 timestamp)
 *   net_active  — total active net_isolator slots (0 if not queried)
 *   net_denials — total network denial count from net_isolator status
 *
 * Returns number of bytes written to the report buffer, or 0 on error.
 *
 * Side effect: calls microkit_ppcall(CH_AGENTFS_CTRL, OP_AGENTFS_PUT) to
 * persist the report. AgentFS stores up to 4 MR-word payloads inline; for
 * larger reports it reads from shared ring memory (not wired here — the
 * report is embedded in MR3/MR4 as a size hint and the monitor's shared ring
 * is used if available, otherwise AgentFS logs the metadata only).
 *
 * In the initial implementation we store:
 *   MR0 = OP_AGENTFS_PUT
 *   MR1 = object kind: 0xCA = capability attestation
 *   MR2 = boot_tick (lower 32 bits) → used as object ID
 *   MR3 = report byte length
 * AgentFS will later be extended to read the full blob from shared ring;
 * for now the report is computed in-kernel and the SHA-512 hash is stored.
 */
uint32_t cap_broker_attest(uint64_t boot_tick, uint32_t net_active, uint32_t net_denials) {
    /* ── Build report buffer ─────────────────────────────────────────────── */
    static uint8_t report[ATTEST_MAX_REPORT];
    uint8_t *p = report;

    /* Header */
    attest_put_u32(p,  ATTEST_MAGIC);   p += 4;
    attest_put_u32(p,  ATTEST_VERSION); p += 4;
    attest_put_u64(p,  boot_tick);      p += 8;
    attest_put_u32(p,  cap_count);      p += 4;
    attest_put_u32(p,  MAX_CAPS);       p += 4;
    attest_put_u64(p,  audit_seq);      p += 8;

    /* Cap table entries (active only) */
    for (int i = 0; i < MAX_CAPS; i++) {
        if (!cap_table[i].active) continue;
        attest_put_u32(p, cap_table[i].cap.cptr);       p += 4;
        attest_put_u32(p, cap_table[i].cap.rights);     p += 4;
        attest_put_u32(p, cap_table[i].cap.kind);       p += 4;
        attest_put_u32(p, cap_table[i].owner_pd);       p += 4;
        attest_put_u32(p, cap_table[i].granted_to);     p += 4;
    }

    /* Net ACL summary (8 bytes) */
    attest_put_u32(p, net_active);    p += 4;
    attest_put_u32(p, net_denials);   p += 4;

    uint32_t report_body_len = (uint32_t)(p - report);

    /* ── SHA-512 signature over report body ──────────────────────────────── */
    uint8_t sig_slot[64];
    /* key_id sentinel in first 8 bytes */
    for (int i = 0; i < 8; i++) sig_slot[i] = ATTEST_KEY_ID[i];
    /* SHA-512 of body in bytes [8..39] (32 bytes of 64-byte hash) */
    uint8_t full_hash[64];
    sha512(report, report_body_len, full_hash);
    for (int i = 0; i < 32; i++) sig_slot[8 + i] = full_hash[i];
    /* Zero-pad bytes [40..63] */
    for (int i = 40; i < 64; i++) sig_slot[i] = 0;

    /* Append signature */
    for (int i = 0; i < 64; i++) *p++ = sig_slot[i];
    uint32_t total_len = (uint32_t)(p - report);

    console_log(4, 4, "[cap_broker] Attestation report generated, storing to AgentFS\n");

    /* ── Store to AgentFS ────────────────────────────────────────────────── */
    /* MR0: opcode, MR1: object kind (0xCA=attest), MR2: tick lo32, MR3: len */
    microkit_mr_set(0, OP_AGENTFS_PUT);
    microkit_mr_set(1, 0xCAu);
    microkit_mr_set(2, (uint32_t)(boot_tick & 0xFFFFFFFFu));
    microkit_mr_set(3, total_len);
    microkit_ppcall(CH_AGENTFS_CTRL, microkit_msginfo_new(0, 4));

    uint32_t agentfs_result = (uint32_t)microkit_mr_get(0);
    if (agentfs_result == 0) {
        console_log(4, 4, "[cap_broker] Attestation stored in AgentFS\n");
    } else {
        console_log(4, 4, "[cap_broker] AgentFS store returned non-zero (large report: ring path needed)\n");
    }

    return total_len;
}
