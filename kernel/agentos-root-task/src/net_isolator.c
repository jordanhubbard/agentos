/*
 * agentOS net_isolator — per-agent outbound network firewall via seL4 capability model
 *
 * Passive PD, priority 160.
 *
 * Sits between agent slots and net_svc as a proxy/firewall.
 * Each agent slot has an ACL table of allowed hostname:port pairs.
 * Connections not matching the ACL are denied and logged to cap_audit_log.
 *
 * Protocol:
 *   OP_NET_CONNECT  (0x70) — MR1=slot_id, MR2=host_hash, MR3=port:
 *                            Check ACL → MR0=1 (allowed) or 0 (denied).
 *                            On deny: notify cap_audit_log.
 *   OP_NET_ACL_SET  (0x71) — MR1=slot_id, MR2=rule_idx, MR3=port,
 *                            shared ring encodes host string at given offset.
 *                            Adds an allow rule for the slot.
 *   OP_NET_ACL_GET  (0x72) — MR1=slot_id → MR0=rule_count, MR1=deny_count,
 *                            MR2=conn_count, MR3=slot_active
 *   OP_NET_ACL_CLEAR (0x73) — MR1=slot_id: remove all ACL entries for slot.
 *                             Called on slot teardown.
 *   OP_NET_STATUS   (0x74) — Returns MR0=total_slots_active, MR1=total_denials,
 *                            MR2=total_connections, MR3=magic.
 *
 * ACL rule format:
 *   host: up to NET_MAX_HOST_LEN (63) bytes, NUL-terminated.
 *         Wildcard "*" matches any host.
 *         Prefix "*.example.com" matches any subdomain.
 *   port: 0 means any port (wildcard). 1-65535 exact match.
 *
 * Channels (local IDs from net_isolator's perspective):
 *   id=0 CH_IN:     incoming PPCs from controller / workers (pp=true)
 *   id=1 CH_AUDIT:  notify cap_audit_log on deny event
 *
 * Shared memory (256KB net_isolator_ring):
 *   Header + per-slot ACL tables + status counters, readable by controller.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/net_isolator_contract.h"

/* ── Opcodes ─────────────────────────────────────────────────────────────── */
#define OP_NET_CONNECT   0x70u
#define OP_NET_ACL_SET   0x71u
#define OP_NET_ACL_GET   0x72u
#define OP_NET_ACL_CLEAR 0x73u
#define OP_NET_STATUS    0x74u

/* ── Channel local IDs ──────────────────────────────────────────────────── */
#define CH_IN    0
#define CH_AUDIT 1

/* ── Limits ─────────────────────────────────────────────────────────────── */
#define NET_MAX_SLOTS       8    /* matches VIBE_MAX_SLOTS */
#define NET_MAX_RULES       16   /* max ACL rules per slot */
#define NET_MAX_HOST_LEN    63   /* max hostname length (not incl NUL) */

/* ── Magic for ring header ───────────────────────────────────────────────── */
#define NET_RING_MAGIC  0x4E457449u  /* "NEtI" */

/* ── ACL rule ────────────────────────────────────────────────────────────── */
typedef struct {
    char     host[NET_MAX_HOST_LEN + 1];  /* hostname or "*" or "*.domain" */
    uint16_t port;                         /* 0 = any */
    uint8_t  active;                       /* 1 = in use */
    uint8_t  _pad;
} net_acl_rule_t;

/* ── Per-slot state ──────────────────────────────────────────────────────── */
typedef struct {
    uint32_t       slot_id;
    uint32_t       active;          /* 1 = registered */
    uint32_t       deny_count;
    uint32_t       conn_count;
    net_acl_rule_t rules[NET_MAX_RULES];
} net_slot_t;

static net_slot_t slots[NET_MAX_SLOTS];

/* Global statistics */
static uint32_t total_denials    = 0;
static uint32_t total_connections = 0;
static uint32_t total_active     = 0;

/* ── Shared memory (256KB status ring) ───────────────────────────────────── */
uintptr_t net_isolator_ring_vaddr;

#define NI_RING_BASE  ((volatile uint8_t *)net_isolator_ring_vaddr)
#define NI_RING_SIZE  0x40000u  /* 256KB */

/* ── Debug helpers ───────────────────────────────────────────────────────── */
static void ni_puts(const char *s) { microkit_dbg_puts(s); }

static void ni_put_dec(uint32_t v) {
    if (v == 0) { ni_puts("0"); return; }
    char buf[12]; int i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    ni_puts(&buf[i]);
}

/* ── String helpers (no libc) ────────────────────────────────────────────── */
static uint32_t ni_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int ni_strncmp(const char *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static void ni_strncpy(char *dst, const char *src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ── ACL matching ────────────────────────────────────────────────────────── */

/* Simple FNV-1a hash for host matching (32-bit) */
static uint32_t fnv1a(const char *s) {
    uint32_t hash = 0x811c9dc5u;
    while (*s) {
        hash ^= (uint32_t)(unsigned char)*s++;
        hash *= 0x01000193u;
    }
    return hash;
}

/*
 * Match hostname against a rule pattern.
 * Patterns: "*" = any, "*.domain.com" = any subdomain, exact otherwise.
 */
static int ni_host_match(const char *pattern, const char *host) {
    if (pattern[0] == '*' && pattern[1] == '\0')
        return 1;  /* wildcard: match everything */

    if (pattern[0] == '*' && pattern[1] == '.') {
        /* Subdomain wildcard: *.example.com */
        const char *suffix = pattern + 1;  /* ".example.com" */
        uint32_t hlen = ni_strlen(host);
        uint32_t slen = ni_strlen(suffix);
        if (hlen < slen) return 0;
        return ni_strncmp(host + (hlen - slen), suffix, slen) == 0;
    }

    /* Exact match */
    return ni_strncmp(pattern, host, NET_MAX_HOST_LEN + 1) == 0;
}

/*
 * Look up slot by slot_id. Returns pointer or NULL.
 */
static net_slot_t *find_slot(uint32_t slot_id) {
    for (int i = 0; i < NET_MAX_SLOTS; i++) {
        if (slots[i].active && slots[i].slot_id == slot_id)
            return &slots[i];
    }
    return NULL;
}

/*
 * Find or allocate a slot entry. Returns pointer or NULL if table full.
 */
static net_slot_t *get_or_alloc_slot(uint32_t slot_id) {
    net_slot_t *s = find_slot(slot_id);
    if (s) return s;

    for (int i = 0; i < NET_MAX_SLOTS; i++) {
        if (!slots[i].active) {
            slots[i].slot_id    = slot_id;
            slots[i].active     = 1;
            slots[i].deny_count  = 0;
            slots[i].conn_count  = 0;
            for (int j = 0; j < NET_MAX_RULES; j++)
                slots[i].rules[j].active = 0;
            total_active++;
            return &slots[i];
        }
    }
    return NULL;
}

/* ── Ring buffer update ──────────────────────────────────────────────────── */
static void update_ring(void) {
    volatile uint8_t *r = NI_RING_BASE;
    uint32_t p = 0;

    /* Write magic header */
    r[p++] = (NET_RING_MAGIC >>  0) & 0xFF;
    r[p++] = (NET_RING_MAGIC >>  8) & 0xFF;
    r[p++] = (NET_RING_MAGIC >> 16) & 0xFF;
    r[p++] = (NET_RING_MAGIC >> 24) & 0xFF;

    /* Write global stats (4 x uint32) */
    volatile uint32_t *stats = (volatile uint32_t *)(r + p);
    stats[0] = total_active;
    stats[1] = total_denials;
    stats[2] = total_connections;
    stats[3] = (uint32_t)NET_MAX_SLOTS;
    p += 16;

    /* Per-slot snapshot: slot_id(4) + active(4) + deny_count(4) + conn_count(4)
       + rule_count(4) + _pad(12) = 32 bytes per slot */
    for (int i = 0; i < NET_MAX_SLOTS; i++) {
        volatile uint32_t *sv = (volatile uint32_t *)(r + p);
        sv[0] = slots[i].slot_id;
        sv[1] = slots[i].active;
        sv[2] = slots[i].deny_count;
        sv[3] = slots[i].conn_count;
        /* Count active rules */
        uint32_t rc = 0;
        for (int j = 0; j < NET_MAX_RULES; j++)
            if (slots[i].rules[j].active) rc++;
        sv[4] = rc;
        sv[5] = 0; sv[6] = 0; sv[7] = 0;
        p += 32;
    }
}

/* ── Audit log notification ──────────────────────────────────────────────── */
static void audit_deny(uint32_t slot_id, uint32_t host_hash, uint32_t port) {
    /* Send denial event to cap_audit_log via CH_AUDIT */
    /* Format: MR0=DENY_EVENT, MR1=slot_id, MR2=host_hash, MR3=port */
    microkit_mr_set(0, 0x7001u);   /* NET_DENY_EVENT type */
    microkit_mr_set(1, slot_id);
    microkit_mr_set(2, host_hash);
    microkit_mr_set(3, port);
    microkit_notify(CH_AUDIT);
}

/* ── Initialization ──────────────────────────────────────────────────────── */
void init(void) {
    ni_puts("[net_isolator] init: passive PD priority 160\n");

    for (int i = 0; i < NET_MAX_SLOTS; i++) {
        slots[i].active     = 0;
        slots[i].slot_id    = 0;
        slots[i].deny_count  = 0;
        slots[i].conn_count  = 0;
        for (int j = 0; j < NET_MAX_RULES; j++)
            slots[i].rules[j].active = 0;
    }

    update_ring();
    ni_puts("[net_isolator] ACL tables cleared, ring initialised\n");
}

/* ── Protected procedure call handler ───────────────────────────────────── */
seL4_MessageInfo_t protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    (void)msginfo;
    uint32_t op      = (uint32_t)microkit_mr_get(0);
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);

    switch (op) {

    /* ── OP_NET_CONNECT: check ACL ────────────────────────────────────────── */
    case OP_NET_CONNECT: {
        uint32_t host_hash = (uint32_t)microkit_mr_get(2);
        uint32_t port      = (uint32_t)microkit_mr_get(3);

        net_slot_t *s = find_slot(slot_id);
        if (!s) {
            /* Unknown slot — deny by default */
            total_denials++;
            microkit_mr_set(0, 0);  /* denied */
            audit_deny(slot_id, host_hash, port);
            ni_puts("[net_isolator] DENY: unknown slot ");
            ni_put_dec(slot_id);
            ni_puts("\n");
            return microkit_msginfo_new(0, 1);
        }

        /* Scan ACL rules */
        int allowed = 0;
        for (int i = 0; i < NET_MAX_RULES && !allowed; i++) {
            net_acl_rule_t *r = &s->rules[i];
            if (!r->active) continue;

            /* Port check: 0 = any, else exact */
            int port_ok = (r->port == 0) || (r->port == (uint16_t)port);
            if (!port_ok) continue;

            /* Host check via hash comparison (fast path) */
            uint32_t rule_hash = fnv1a(r->host);
            if (r->host[0] == '*') {
                /* Wildcard — always pass host check (port already checked) */
                allowed = 1;
            } else if (rule_hash == host_hash) {
                allowed = 1;
            }
        }

        if (allowed) {
            s->conn_count++;
            total_connections++;
            microkit_mr_set(0, 1);  /* allowed */
        } else {
            s->deny_count++;
            total_denials++;
            microkit_mr_set(0, 0);  /* denied */
            audit_deny(slot_id, host_hash, port);
            ni_puts("[net_isolator] DENY: slot=");
            ni_put_dec(slot_id);
            ni_puts(" port=");
            ni_put_dec(port);
            ni_puts("\n");
        }

        update_ring();
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_NET_ACL_SET: add allow rule ───────────────────────────────────── */
    case OP_NET_ACL_SET: {
        uint32_t rule_idx = (uint32_t)microkit_mr_get(2);
        uint32_t port     = (uint32_t)microkit_mr_get(3);

        if (rule_idx >= NET_MAX_RULES) {
            microkit_mr_set(0, 0);
            return microkit_msginfo_new(0, 1);
        }

        net_slot_t *s = get_or_alloc_slot(slot_id);
        if (!s) {
            microkit_mr_set(0, 0);  /* table full */
            return microkit_msginfo_new(0, 1);
        }

        /*
         * Host string is encoded in the shared ring at a fixed offset
         * (NET_HOST_RING_OFFSET + slot_id * NET_MAX_RULES * 64 + rule_idx * 64).
         * Controller writes the host string there before calling OP_NET_ACL_SET.
         */
        uint32_t host_off = 512 + slot_id * NET_MAX_RULES * 64 + rule_idx * 64;
        const char *host_src = (const char *)(net_isolator_ring_vaddr + host_off);

        ni_strncpy(s->rules[rule_idx].host, host_src, NET_MAX_HOST_LEN + 1);
        s->rules[rule_idx].port   = (uint16_t)(port & 0xFFFF);
        s->rules[rule_idx].active = 1;

        ni_puts("[net_isolator] ACL_SET slot=");
        ni_put_dec(slot_id);
        ni_puts(" rule=");
        ni_put_dec(rule_idx);
        ni_puts(" host=");
        ni_puts(s->rules[rule_idx].host);
        ni_puts(" port=");
        ni_put_dec(port);
        ni_puts("\n");

        update_ring();
        microkit_mr_set(0, 1);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_NET_ACL_GET: query slot state ─────────────────────────────────── */
    case OP_NET_ACL_GET: {
        net_slot_t *s = find_slot(slot_id);
        if (!s) {
            microkit_mr_set(0, 0);
            microkit_mr_set(1, 0);
            microkit_mr_set(2, 0);
            microkit_mr_set(3, 0);
            return microkit_msginfo_new(0, 4);
        }

        uint32_t rule_count = 0;
        for (int i = 0; i < NET_MAX_RULES; i++)
            if (s->rules[i].active) rule_count++;

        microkit_mr_set(0, rule_count);
        microkit_mr_set(1, s->deny_count);
        microkit_mr_set(2, s->conn_count);
        microkit_mr_set(3, s->active);
        return microkit_msginfo_new(0, 4);
    }

    /* ── OP_NET_ACL_CLEAR: remove all rules for slot ─────────────────────── */
    case OP_NET_ACL_CLEAR: {
        net_slot_t *s = find_slot(slot_id);
        if (s) {
            for (int i = 0; i < NET_MAX_RULES; i++)
                s->rules[i].active = 0;
            s->active     = 0;
            s->deny_count  = 0;
            s->conn_count  = 0;
            if (total_active > 0) total_active--;
            ni_puts("[net_isolator] ACL_CLEAR slot=");
            ni_put_dec(slot_id);
            ni_puts("\n");
        }
        update_ring();
        microkit_mr_set(0, 1);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_NET_STATUS: global stats ─────────────────────────────────────── */
    case OP_NET_STATUS: {
        microkit_mr_set(0, total_active);
        microkit_mr_set(1, total_denials);
        microkit_mr_set(2, total_connections);
        microkit_mr_set(3, NET_RING_MAGIC);
        return microkit_msginfo_new(0, 4);
    }

    default:
        microkit_mr_set(0, 0xFFFFFFFFu);  /* unknown op */
        return microkit_msginfo_new(0, 1);
    }
}

/* ── Notification handler ────────────────────────────────────────────────── */
void notified(microkit_channel ch) {
    /* net_isolator is passive; no inbound notifications expected.
       Log unexpected notifies for debugging. */
    ni_puts("[net_isolator] unexpected notify ch=");
    ni_put_dec((uint32_t)ch);
    ni_puts("\n");
}
