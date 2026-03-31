/*
 * agentOS Quota Enforcement Protection Domain
 *
 * Passive PD (priority 115) that enforces per-agent resource quotas:
 *   - CPU time budget (milliseconds)
 *   - Memory usage limit (kilobytes)
 *
 * When an agent exceeds its quota, quota_pd posts MSG_CAP_REVOKE to the
 * cap_broker (via controller channel), effectively sandboxing the offending
 * agent by stripping its capabilities. The event is also logged.
 *
 * IPC operations:
 *   OP_QUOTA_REGISTER (0x60) — register an agent slot with cpu/mem limits
 *   OP_QUOTA_TICK     (0x61) — accumulate CPU tick for an agent (called per sched round)
 *   OP_QUOTA_STATUS   (0x62) — query quota state for an agent
 *   OP_QUOTA_SET      (0x63) — update quota limits for an existing agent
 *
 * Channels (from quota_pd perspective):
 *   id=0: controller PPCs in (register/tick/status/set)
 *   id=1: init_agent PPCs in (register on spawn, tick per round)
 *   id=2: quota_pd -> cap_broker (notify: post MSG_CAP_REVOKE on exceeded)
 *
 * Memory:
 *   quota_ring (256KB shared MR): quota table + event log
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* ── Opcodes ──────────────────────────────────────────────────────────────── */
#define OP_QUOTA_REGISTER  0x60  /* Register agent: MR1=agent_id, MR2=cpu_ms, MR3=mem_kb */
#define OP_QUOTA_TICK      0x61  /* Tick agent: MR1=agent_id, MR2=cpu_delta_ms, MR3=mem_cur_kb */
#define OP_QUOTA_STATUS    0x62  /* Query: MR1=agent_id → MR0=cpu_used, MR1=cpu_limit, MR2=mem_used, MR3=mem_limit, MR4=flags */
#define OP_QUOTA_SET       0x63  /* Update: MR1=agent_id, MR2=new_cpu_ms, MR3=new_mem_kb */

/* ── Quota table ──────────────────────────────────────────────────────────── */
#define MAX_QUOTA_SLOTS  16

typedef struct {
    uint32_t agent_id;       /* agent/PD identifier */
    uint32_t cpu_limit_ms;   /* max CPU time in milliseconds (0 = unlimited) */
    uint32_t mem_limit_kb;   /* max memory in kilobytes (0 = unlimited) */
    uint32_t cpu_used_ms;    /* accumulated CPU time */
    uint32_t mem_used_kb;    /* current memory usage */
    uint32_t flags;          /* QUOTA_FLAG_* */
    uint64_t tick_count;     /* total ticks received */
    uint64_t exceed_tick;    /* tick at which quota was exceeded (0 = not exceeded) */
} quota_entry_t;

/* ── Event log entry (32 bytes) ───────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t seq;           /* monotonic sequence */
    uint64_t tick;          /* boot tick at event time */
    uint32_t agent_id;      /* affected agent */
    uint32_t event_type;    /* 1=register, 2=cpu_exceed, 3=mem_exceed, 4=revoked, 5=set */
    uint32_t value_a;       /* context-dependent (e.g. cpu_used_ms) */
    uint32_t value_b;       /* context-dependent (e.g. cpu_limit_ms) */
} quota_event_t;

/* ── Shared memory layout ─────────────────────────────────────────────────── */
/*
 * quota_ring (256KB = 0x40000):
 *   [0..63]           — header (64 bytes)
 *   [64..64+16*48-1]  — quota table (16 slots × 48 bytes = 768 bytes)
 *   [832..]           — event log ring (remaining space / 32 bytes per entry)
 */

typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0x0107A00D ("QUOTA OOD") */
    uint32_t version;       /* 1 */
    uint32_t slot_count;    /* MAX_QUOTA_SLOTS */
    uint32_t active_count;  /* number of active slots */
    uint64_t event_head;    /* next write index in event log */
    uint64_t event_count;   /* total events logged */
    uint64_t event_capacity;/* number of event slots */
    uint8_t  _pad[24];     /* pad to 64 bytes */
} quota_header_t;

#define QUOTA_MAGIC  0x0107A00D

uintptr_t quota_ring_vaddr;

#define QUOTA_HDR     ((volatile quota_header_t *)quota_ring_vaddr)
#define QUOTA_TABLE   ((volatile quota_entry_t *) \
    ((uint8_t *)quota_ring_vaddr + sizeof(quota_header_t)))
#define QUOTA_EVENTS  ((volatile quota_event_t *) \
    ((uint8_t *)quota_ring_vaddr + sizeof(quota_header_t) + \
     MAX_QUOTA_SLOTS * sizeof(quota_entry_t)))

/* Channel IDs from quota_pd's perspective */
#define QP_CH_CONTROLLER  0   /* controller queries/ticks */
#define QP_CH_INIT_AGENT  1   /* init_agent register/ticks */
#define QP_CH_CAP_BROKER  2   /* notify cap_broker for revocation */

static uint64_t boot_tick = 0;

/* ── Helper: decimal print ────────────────────────────────────────────────── */
static void put_dec(uint32_t v) {
    console_log(14, 14, "0");
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    }
    console_log(14, 14, &buf[i]);
}

/* ── Init ─────────────────────────────────────────────────────────────────── */
static void quota_pd_init(void) {
    volatile quota_header_t *hdr = QUOTA_HDR;

    uint64_t region_size = 0x40000;  /* 256KB */
    uint64_t table_size = sizeof(quota_header_t) + MAX_QUOTA_SLOTS * sizeof(quota_entry_t);
    uint64_t event_space = region_size - table_size;
    uint64_t event_cap = event_space / sizeof(quota_event_t);

    hdr->magic          = QUOTA_MAGIC;
    hdr->version        = 1;
    hdr->slot_count     = MAX_QUOTA_SLOTS;
    hdr->active_count   = 0;
    hdr->event_head     = 0;
    hdr->event_count    = 0;
    hdr->event_capacity = event_cap;

    /* Clear quota table */
    volatile quota_entry_t *table = QUOTA_TABLE;
    for (int i = 0; i < MAX_QUOTA_SLOTS; i++) {
        table[i].agent_id     = 0;
        table[i].cpu_limit_ms = 0;
        table[i].mem_limit_kb = 0;
        table[i].cpu_used_ms  = 0;
        table[i].mem_used_kb  = 0;
        table[i].flags        = 0;
        table[i].tick_count   = 0;
        table[i].exceed_tick  = 0;
    }

    console_log(14, 14, "[quota_pd] Initialized: ");
    put_dec(MAX_QUOTA_SLOTS);
    console_log(14, 14, " slots, ");
    put_dec((uint32_t)event_cap);
    console_log(14, 14, " event entries\n");
}

/* ── Append event to log ──────────────────────────────────────────────────── */
static void quota_log_event(uint32_t agent_id, uint32_t event_type,
                             uint32_t value_a, uint32_t value_b) {
    volatile quota_header_t *hdr = QUOTA_HDR;
    volatile quota_event_t *events = QUOTA_EVENTS;

    uint64_t idx = hdr->event_head % hdr->event_capacity;
    volatile quota_event_t *e = &events[idx];

    e->seq        = hdr->event_count;
    e->tick       = boot_tick;
    e->agent_id   = agent_id;
    e->event_type = event_type;
    e->value_a    = value_a;
    e->value_b    = value_b;

    hdr->event_head = (hdr->event_head + 1) % hdr->event_capacity;
    hdr->event_count++;
}

/* ── Find slot by agent_id ────────────────────────────────────────────────── */
static int find_slot(uint32_t agent_id) {
    volatile quota_entry_t *table = QUOTA_TABLE;
    for (int i = 0; i < MAX_QUOTA_SLOTS; i++) {
        if ((table[i].flags & QUOTA_FLAG_ACTIVE) && table[i].agent_id == agent_id) {
            return i;
        }
    }
    return -1;
}

/* ── Find free slot ───────────────────────────────────────────────────────── */
static int find_free_slot(void) {
    volatile quota_entry_t *table = QUOTA_TABLE;
    for (int i = 0; i < MAX_QUOTA_SLOTS; i++) {
        if (!(table[i].flags & QUOTA_FLAG_ACTIVE)) {
            return i;
        }
    }
    return -1;
}

/* ── Revoke agent capabilities via cap_broker ─────────────────────────────── */
static void revoke_agent_caps(uint32_t agent_id, uint32_t reason_flag) {
    /*
     * Notify controller/cap_broker channel to revoke capabilities.
     * MR0: MSG_QUOTA_REVOKE tag
     * MR1: agent_id
     * MR2: reason (QUOTA_FLAG_CPU_EXCEED or QUOTA_FLAG_MEM_EXCEED)
     *
     * Since quota_pd is passive (priority 115), we use notify (not PPC)
     * to the controller which then calls cap_broker_revoke() on our behalf.
     */
    microkit_mr_set(0, MSG_QUOTA_REVOKE);
    microkit_mr_set(1, agent_id);
    microkit_mr_set(2, reason_flag);
    microkit_notify(QP_CH_CAP_BROKER);

    console_log(14, 14, "[quota_pd] REVOKE agent=");
    put_dec(agent_id);
    console_log(14, 14, " reason=");
    put_dec(reason_flag);
    console_log(14, 14, "\n");
}

/* ── Check quota and enforce ──────────────────────────────────────────────── */
static void check_and_enforce(int slot_idx) {
    volatile quota_entry_t *entry = &QUOTA_TABLE[slot_idx];

    /* Already revoked — skip */
    if (entry->flags & QUOTA_FLAG_REVOKED) return;

    bool exceeded = false;

    /* Check CPU quota */
    if (entry->cpu_limit_ms > 0 && entry->cpu_used_ms >= entry->cpu_limit_ms) {
        if (!(entry->flags & QUOTA_FLAG_CPU_EXCEED)) {
            entry->flags |= QUOTA_FLAG_CPU_EXCEED;
            exceeded = true;
            quota_log_event(entry->agent_id, 2, entry->cpu_used_ms, entry->cpu_limit_ms);
            console_log(14, 14, "[quota_pd] CPU quota exceeded: agent=");
            put_dec(entry->agent_id);
            console_log(14, 14, " used=");
            put_dec(entry->cpu_used_ms);
            console_log(14, 14, "ms limit=");
            put_dec(entry->cpu_limit_ms);
            console_log(14, 14, "ms\n");
        }
    }

    /* Check memory quota */
    if (entry->mem_limit_kb > 0 && entry->mem_used_kb >= entry->mem_limit_kb) {
        if (!(entry->flags & QUOTA_FLAG_MEM_EXCEED)) {
            entry->flags |= QUOTA_FLAG_MEM_EXCEED;
            exceeded = true;
            quota_log_event(entry->agent_id, 3, entry->mem_used_kb, entry->mem_limit_kb);
            console_log(14, 14, "[quota_pd] MEM quota exceeded: agent=");
            put_dec(entry->agent_id);
            console_log(14, 14, " used=");
            put_dec(entry->mem_used_kb);
            console_log(14, 14, "kb limit=");
            put_dec(entry->mem_limit_kb);
            console_log(14, 14, "kb\n");
        }
    }

    /* If newly exceeded, revoke caps */
    if (exceeded) {
        entry->flags |= QUOTA_FLAG_REVOKED;
        entry->exceed_tick = boot_tick;
        uint32_t reason = 0;
        if (entry->flags & QUOTA_FLAG_CPU_EXCEED) reason |= QUOTA_FLAG_CPU_EXCEED;
        if (entry->flags & QUOTA_FLAG_MEM_EXCEED) reason |= QUOTA_FLAG_MEM_EXCEED;
        revoke_agent_caps(entry->agent_id, reason);
        quota_log_event(entry->agent_id, 4, reason, (uint32_t)boot_tick);
    }
}

/* ── Handle PPC operations ────────────────────────────────────────────────── */
static microkit_msginfo handle_request(microkit_msginfo msg) {
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {

    case OP_QUOTA_REGISTER: {
        uint32_t agent_id    = (uint32_t)microkit_mr_get(1);
        uint32_t cpu_limit   = (uint32_t)microkit_mr_get(2);
        uint32_t mem_limit   = (uint32_t)microkit_mr_get(3);

        /* Check if already registered */
        int existing = find_slot(agent_id);
        if (existing >= 0) {
            console_log(14, 14, "[quota_pd] WARN: agent already registered, slot=");
            put_dec((uint32_t)existing);
            console_log(14, 14, "\n");
            microkit_mr_set(0, (uint32_t)existing);
            microkit_mr_set(1, 1);  /* already exists */
            return microkit_msginfo_new(0, 2);
        }

        int slot = find_free_slot();
        if (slot < 0) {
            console_log(14, 14, "[quota_pd] ERROR: quota table full\n");
            microkit_mr_set(0, 0xFFFFFFFF);
            microkit_mr_set(1, 0xE1);  /* ERR_TABLE_FULL */
            return microkit_msginfo_new(0, 2);
        }

        volatile quota_entry_t *entry = &QUOTA_TABLE[slot];
        entry->agent_id     = agent_id;
        entry->cpu_limit_ms = cpu_limit;
        entry->mem_limit_kb = mem_limit;
        entry->cpu_used_ms  = 0;
        entry->mem_used_kb  = 0;
        entry->flags        = QUOTA_FLAG_ACTIVE;
        entry->tick_count   = 0;
        entry->exceed_tick  = 0;

        QUOTA_HDR->active_count++;

        quota_log_event(agent_id, 1, cpu_limit, mem_limit);

        console_log(14, 14, "[quota_pd] Registered agent=");
        put_dec(agent_id);
        console_log(14, 14, " cpu=");
        put_dec(cpu_limit);
        console_log(14, 14, "ms mem=");
        put_dec(mem_limit);
        console_log(14, 14, "kb slot=");
        put_dec((uint32_t)slot);
        console_log(14, 14, "\n");

        microkit_mr_set(0, (uint32_t)slot);
        microkit_mr_set(1, 0);  /* success */
        return microkit_msginfo_new(0, 2);
    }

    case OP_QUOTA_TICK: {
        uint32_t agent_id     = (uint32_t)microkit_mr_get(1);
        uint32_t cpu_delta_ms = (uint32_t)microkit_mr_get(2);
        uint32_t mem_cur_kb   = (uint32_t)microkit_mr_get(3);

        int slot = find_slot(agent_id);
        if (slot < 0) {
            microkit_mr_set(0, 0xE2);  /* ERR_NOT_FOUND */
            return microkit_msginfo_new(0, 1);
        }

        volatile quota_entry_t *entry = &QUOTA_TABLE[slot];
        entry->cpu_used_ms += cpu_delta_ms;
        entry->mem_used_kb  = mem_cur_kb;
        entry->tick_count++;

        /* Check and enforce quotas */
        check_and_enforce(slot);

        microkit_mr_set(0, 0);  /* success */
        microkit_mr_set(1, entry->flags);
        return microkit_msginfo_new(0, 2);
    }

    case OP_QUOTA_STATUS: {
        uint32_t agent_id = (uint32_t)microkit_mr_get(1);

        int slot = find_slot(agent_id);
        if (slot < 0) {
            microkit_mr_set(0, 0xE2);  /* ERR_NOT_FOUND */
            return microkit_msginfo_new(0, 1);
        }

        volatile quota_entry_t *entry = &QUOTA_TABLE[slot];
        microkit_mr_set(0, entry->cpu_used_ms);
        microkit_mr_set(1, entry->cpu_limit_ms);
        microkit_mr_set(2, entry->mem_used_kb);
        microkit_mr_set(3, entry->mem_limit_kb);
        microkit_mr_set(4, entry->flags);
        return microkit_msginfo_new(0, 5);
    }

    case OP_QUOTA_SET: {
        uint32_t agent_id   = (uint32_t)microkit_mr_get(1);
        uint32_t new_cpu_ms = (uint32_t)microkit_mr_get(2);
        uint32_t new_mem_kb = (uint32_t)microkit_mr_get(3);

        int slot = find_slot(agent_id);
        if (slot < 0) {
            microkit_mr_set(0, 0xE2);  /* ERR_NOT_FOUND */
            return microkit_msginfo_new(0, 1);
        }

        volatile quota_entry_t *entry = &QUOTA_TABLE[slot];
        entry->cpu_limit_ms = new_cpu_ms;
        entry->mem_limit_kb = new_mem_kb;

        /* Clear exceeded flags if new limits are more generous */
        if (new_cpu_ms == 0 || entry->cpu_used_ms < new_cpu_ms) {
            entry->flags &= ~QUOTA_FLAG_CPU_EXCEED;
        }
        if (new_mem_kb == 0 || entry->mem_used_kb < new_mem_kb) {
            entry->flags &= ~QUOTA_FLAG_MEM_EXCEED;
        }
        /* Clear revoked if neither exceeded anymore */
        if (!(entry->flags & (QUOTA_FLAG_CPU_EXCEED | QUOTA_FLAG_MEM_EXCEED))) {
            entry->flags &= ~QUOTA_FLAG_REVOKED;
        }

        quota_log_event(agent_id, 5, new_cpu_ms, new_mem_kb);

        console_log(14, 14, "[quota_pd] Updated agent=");
        put_dec(agent_id);
        console_log(14, 14, " cpu=");
        put_dec(new_cpu_ms);
        console_log(14, 14, "ms mem=");
        put_dec(new_mem_kb);
        console_log(14, 14, "kb\n");

        microkit_mr_set(0, 0);  /* success */
        microkit_mr_set(1, entry->flags);
        return microkit_msginfo_new(0, 2);
    }

    default:
        console_log(14, 14, "[quota_pd] WARN: unknown opcode\n");
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }

    (void)msg;
}

/* ── Microkit entry points ────────────────────────────────────────────────── */
void init(void) {
    agentos_log_boot("quota_pd");
    quota_pd_init();
    console_log(14, 14, "[quota_pd] Ready — priority 115, passive, ");
    put_dec(MAX_QUOTA_SLOTS);
    console_log(14, 14, " agent quota slots\n");
}

microkit_msginfo protected(microkit_channel channel, microkit_msginfo msg) {
    (void)channel;
    boot_tick++;
    return handle_request(msg);
}

void notified(microkit_channel ch) {
    (void)ch;
    boot_tick++;
}
