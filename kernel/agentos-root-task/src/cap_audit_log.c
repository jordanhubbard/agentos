/*
 * agentOS Capability Audit Log Protection Domain
 *
 * Passive PD (priority 120) that records all capability grant/revoke events
 * in a shared-memory ring buffer. Provides a queryable audit trail for:
 *   - Which capabilities were granted to which agents
 *   - When grants/revokes occurred (by boot tick counter)
 *   - Which WASM capability manifest classes were involved
 *
 * IPC operations:
 *   OP_CAP_LOG        (0x50) — append a grant/revoke event
 *   OP_CAP_LOG_STATUS (0x51) — query ring buffer state
 *   OP_CAP_LOG_DUMP   (0x52) — read entries from ring buffer
 *   OP_CAP_ATTEST     (0x53) — generate signed capability attestation report
 *
 * Callers:
 *   - controller (via cap_broker_grant/revoke → channel 70)
 *   - init_agent (via agent_pool_spawn → channel 71)
 *
 * Memory:
 *   - cap_audit_ring (256KB shared MR): ring buffer for audit entries
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "ed25519_verify.h"  /* sha512() — used by attestation response */

/* Forward declaration: cap_broker_attest lives in cap_broker.c (same compilation unit via monitor.elf) */
uint32_t cap_broker_attest(uint64_t boot_tick, uint32_t net_active, uint32_t net_denials);

/* ── Opcodes ─────────────────────────────────────────────────────────────── */
#define OP_CAP_LOG        0x50  /* Log a cap event: MR0=op, MR1=type, MR2=agent_id, MR3=caps_mask */
#define OP_CAP_LOG_STATUS 0x51  /* Query: returns MR0=count, MR1=head, MR2=capacity, MR3=drops */
#define OP_CAP_LOG_DUMP   0x52  /* Read: MR1=start_idx, MR2=count → entries in shared MR */
#define OP_CAP_ATTEST     0x53  /* Attestation: MR1=boot_tick_lo, MR2=boot_tick_hi,
                                 *              MR3=net_active, MR4=net_denials
                                 * Returns: MR0=report_len, MR1=sha512_hash[0:4] (first 4 bytes) */

/* ── Event types ─────────────────────────────────────────────────────────── */
#define CAP_EVENT_GRANT   1
#define CAP_EVENT_REVOKE  2

/* ── Capability class bitmask (mirrors WASM capability manifest) ─────────── */
#define CAP_CLASS_FS      (1 << 0)   /* filesystem access */
#define CAP_CLASS_NET     (1 << 1)   /* network access */
#define CAP_CLASS_GPU     (1 << 2)   /* GPU compute */
#define CAP_CLASS_IPC     (1 << 3)   /* inter-agent IPC */
#define CAP_CLASS_TIMER   (1 << 4)   /* timer/clock access */
#define CAP_CLASS_STDIO   (1 << 5)   /* stdout/stderr */
#define CAP_CLASS_SPAWN   (1 << 6)   /* spawn child agents */
#define CAP_CLASS_SWAP    (1 << 7)   /* hot-swap proposals */

/* ── Audit log entry (32 bytes, fits nicely in cache line) ───────────────── */
typedef struct __attribute__((packed)) {
    uint64_t seq;          /* monotonic sequence number */
    uint64_t tick;         /* boot tick counter at event time */
    uint32_t event_type;   /* CAP_EVENT_GRANT or CAP_EVENT_REVOKE */
    uint32_t agent_id;     /* target agent/PD id */
    uint32_t caps_mask;    /* bitmask of capability classes */
    uint32_t slot_id;      /* swap slot or worker slot id (0 if N/A) */
} cap_audit_entry_t;

/* ── Ring buffer header (64 bytes, at start of shared memory) ────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;        /* 0xCA9A0D17 ("CAP AUDIT") */
    uint32_t version;      /* 1 */
    uint64_t capacity;     /* number of entry slots */
    uint64_t head;         /* next write index */
    uint64_t count;        /* total events written (may exceed capacity = wrap) */
    uint64_t drops;        /* events dropped (should be 0 with ring overwrite) */
    uint8_t  _pad[24];    /* pad to 64 bytes */
} cap_audit_header_t;

#define CAP_AUDIT_MAGIC  0xCA9A0D17

/* ── Shared memory ───────────────────────────────────────────────────────── */
uintptr_t cap_audit_ring_vaddr;
#define AUDIT_HDR    ((volatile cap_audit_header_t *)cap_audit_ring_vaddr)
#define AUDIT_ENTRIES ((volatile cap_audit_entry_t *) \
    ((uint8_t *)cap_audit_ring_vaddr + sizeof(cap_audit_header_t)))

static uint64_t boot_tick = 0;

/* ── Init ────────────────────────────────────────────────────────────────── */
static void cap_audit_init(void) {
    volatile cap_audit_header_t *hdr = AUDIT_HDR;

    /* Calculate how many entries fit in the region (256KB minus header) */
    uint64_t region_size = 0x40000;  /* 256KB */
    uint64_t entry_space = region_size - sizeof(cap_audit_header_t);
    uint64_t cap = entry_space / sizeof(cap_audit_entry_t);

    hdr->magic    = CAP_AUDIT_MAGIC;
    hdr->version  = 1;
    hdr->capacity = cap;
    hdr->head     = 0;
    hdr->count    = 0;
    hdr->drops    = 0;

    console_log(5, 5, "[cap_audit_log] Ring initialized: ");
    /* Can't printf in Microkit, just log the fact */
    console_log(5, 5, "capacity=8000+ entries, 32B each\n");
}

/* ── Append an audit entry ───────────────────────────────────────────────── */
static void cap_audit_append(uint32_t event_type, uint32_t agent_id,
                              uint32_t caps_mask, uint32_t slot_id) {
    volatile cap_audit_header_t *hdr = AUDIT_HDR;
    volatile cap_audit_entry_t *entries = AUDIT_ENTRIES;

    uint64_t idx = hdr->head;
    volatile cap_audit_entry_t *slot = &entries[idx];

    slot->seq        = hdr->count;
    slot->tick       = boot_tick;
    slot->event_type = event_type;
    slot->agent_id   = agent_id;
    slot->caps_mask  = caps_mask;
    slot->slot_id    = slot_id;

    /* Memory barrier before advancing head */
    __asm__ volatile("" ::: "memory");

    hdr->head = (idx + 1) % hdr->capacity;
    hdr->count++;

    if (event_type == CAP_EVENT_GRANT) {
        console_log(5, 5, "[cap_audit_log] GRANT logged\n");
    } else {
        console_log(5, 5, "[cap_audit_log] REVOKE logged\n");
    }
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void) {
    agentos_log_boot("cap_audit_log");
    cap_audit_init();
    console_log(5, 5, "[cap_audit_log] Ready — passive audit trail\n");
}

/*
 * Protected procedure call handler (passive PD).
 * Called when controller or init_agent PPCs into us.
 *
 * Message register layout depends on opcode:
 *
 * OP_CAP_LOG:
 *   MR0 = opcode (0x50)
 *   MR1 = event_type (1=grant, 2=revoke)
 *   MR2 = agent_id
 *   MR3 = caps_mask (bitmask of CAP_CLASS_*)
 *   MR4 = slot_id (optional, 0 if N/A)
 *
 * OP_CAP_LOG_STATUS:
 *   MR0 = opcode (0x51)
 *   Returns: MR0=total_count, MR1=head, MR2=capacity, MR3=drops
 *
 * OP_CAP_LOG_DUMP:
 *   MR0 = opcode (0x52)
 *   MR1 = start_idx (relative to current head, 0 = most recent)
 *   MR2 = count (max entries to return, capped at 4)
 *   Returns: MR0=actual_count, then 4 MRs per entry (seq, tick, type|agent, caps|slot)
 */
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    (void)msginfo;

    uint64_t op = microkit_mr_get(0);
    boot_tick++;  /* Simple tick counter, incremented per PPC */

    switch (op) {
    case OP_CAP_LOG: {
        uint32_t event_type = (uint32_t)microkit_mr_get(1);
        uint32_t agent_id   = (uint32_t)microkit_mr_get(2);
        uint32_t caps_mask  = (uint32_t)microkit_mr_get(3);
        uint32_t slot_id    = (uint32_t)microkit_mr_get(4);

        if (event_type != CAP_EVENT_GRANT &&
            event_type != CAP_EVENT_REVOKE &&
            event_type != CAP_AUDIT_POLICY_RELOAD) {
            console_log(5, 5, "[cap_audit_log] WARN: invalid event_type\n");
            microkit_mr_set(0, 1);  /* error */
            return microkit_msginfo_new(0, 1);
        }

        cap_audit_append(event_type, agent_id, caps_mask, slot_id);

        if (event_type == CAP_AUDIT_POLICY_RELOAD) {
            console_log(5, 5, "[cap_audit_log] POLICY_RELOAD logged\n");
        }

        microkit_mr_set(0, 0);  /* success */
        return microkit_msginfo_new(0, 1);
    }

    case OP_CAP_LOG_STATUS: {
        volatile cap_audit_header_t *hdr = AUDIT_HDR;
        microkit_mr_set(0, hdr->count);
        microkit_mr_set(1, hdr->head);
        microkit_mr_set(2, hdr->capacity);
        microkit_mr_set(3, hdr->drops);
        return microkit_msginfo_new(0, 4);
    }

    case OP_CAP_LOG_DUMP: {
        volatile cap_audit_header_t *hdr = AUDIT_HDR;
        volatile cap_audit_entry_t *entries = AUDIT_ENTRIES;

        uint32_t start_back = (uint32_t)microkit_mr_get(1);
        uint32_t req_count  = (uint32_t)microkit_mr_get(2);

        /* Cap at 4 entries per call (limited by MR count: 4 MRs per entry = 16 MRs) */
        if (req_count > 4) req_count = 4;

        uint64_t avail = hdr->count < hdr->capacity ? hdr->count : hdr->capacity;
        if (start_back >= avail) {
            microkit_mr_set(0, 0);
            return microkit_msginfo_new(0, 1);
        }

        uint32_t actual = req_count;
        if (start_back + actual > avail) {
            actual = (uint32_t)(avail - start_back);
        }

        microkit_mr_set(0, actual);
        for (uint32_t i = 0; i < actual; i++) {
            /* Walk backwards from head */
            uint64_t idx = (hdr->head + hdr->capacity - 1 - start_back - i) % hdr->capacity;
            volatile cap_audit_entry_t *e = &entries[idx];

            uint32_t mr_base = 1 + i * 4;
            microkit_mr_set(mr_base + 0, e->seq);
            microkit_mr_set(mr_base + 1, e->tick);
            microkit_mr_set(mr_base + 2, ((uint64_t)e->event_type << 32) | e->agent_id);
            microkit_mr_set(mr_base + 3, ((uint64_t)e->caps_mask << 32) | e->slot_id);
        }
        return microkit_msginfo_new(0, 1 + actual * 4);
    }

    case OP_CAP_ATTEST: {
        /*
         * OP_CAP_ATTEST — generate a signed capability attestation report.
         *
         * Input MRs:
         *   MR1 = boot_tick low  32 bits
         *   MR2 = boot_tick high 32 bits (combined: uint64 boot_tick)
         *   MR3 = net_active   (total active net_isolator slots)
         *   MR4 = net_denials  (total denial count from net_isolator status)
         *
         * If MR1=MR2=0, uses the internal boot_tick counter.
         *
         * Returns:
         *   MR0 = report byte length (0 on failure)
         *   MR1 = first 4 bytes of SHA-512 hash (as uint32, little-endian)
         *         useful as a quick fingerprint for the report
         */
        uint32_t tick_lo   = (uint32_t)microkit_mr_get(1);
        uint32_t tick_hi   = (uint32_t)microkit_mr_get(2);
        uint32_t net_act   = (uint32_t)microkit_mr_get(3);
        uint32_t net_deny  = (uint32_t)microkit_mr_get(4);

        uint64_t caller_tick = ((uint64_t)tick_hi << 32) | (uint64_t)tick_lo;
        if (caller_tick == 0ULL) caller_tick = (uint64_t)boot_tick;

        uint32_t report_len = cap_broker_attest(caller_tick, net_act, net_deny);

        /* Return length + first 4 bytes of SHA-512 hash as fingerprint
         * (The full hash is in the report itself; we can't return 64 bytes via MRs.) */
        uint32_t fingerprint = 0;
        /* fingerprint = first 4 bytes of sha512(boot_tick_as_bytes) — simple nonce */
        {
            uint8_t tick_bytes[8];
            tick_bytes[0] = (uint8_t)(caller_tick);
            tick_bytes[1] = (uint8_t)(caller_tick >> 8);
            tick_bytes[2] = (uint8_t)(caller_tick >> 16);
            tick_bytes[3] = (uint8_t)(caller_tick >> 24);
            tick_bytes[4] = (uint8_t)(caller_tick >> 32);
            tick_bytes[5] = (uint8_t)(caller_tick >> 40);
            tick_bytes[6] = (uint8_t)(caller_tick >> 48);
            tick_bytes[7] = (uint8_t)(caller_tick >> 56);
            uint8_t h[64];
            sha512(tick_bytes, 8, h);
            fingerprint = ((uint32_t)h[0]) | ((uint32_t)h[1] << 8) |
                          ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
        }

        microkit_mr_set(0, report_len);
        microkit_mr_set(1, fingerprint);
        return microkit_msginfo_new(0, 2);
    }

    default:
        console_log(5, 5, "[cap_audit_log] WARN: unknown opcode\n");
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }
}

/*
 * Notification handler — cap_audit_log is passive so this is rarely called.
 * If we do get notified, it's a tick signal for the boot_tick counter.
 */
void notified(microkit_channel ch) {
    (void)ch;
    boot_tick++;
}
