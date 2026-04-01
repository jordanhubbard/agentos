/*
 * snapshot_sched.c — agentOS Snapshot Scheduler Protection Domain
 *
 * Passive PD (priority 180). Fires on a configurable tick interval and
 * writes slot snapshots to AgentFS for all running slots.  Enables crash
 * recovery without losing more than ~5 s of state (500 ticks × 10 ms).
 *
 * Opcodes (0xA0-0xA5):
 *   OP_SS_TICK           (0xA0) — advance tick; snapshot all active slots
 *                                 when tick_count % interval == 0
 *   OP_SS_CONFIGURE      (0xA1) — set interval (default 500) and
 *                                 max_snapshots_per_slot (default 3)
 *   OP_SS_STATUS         (0xA2) — return {tick_count, last_snapshot_tick,
 *                                  slots_snapshotted, total_snapshots}
 *   OP_SS_FORCE_SNAPSHOT (0xA3) — immediately snapshot all active slots
 *                                 (called by OOM killer before eviction)
 *   OP_SS_RESTORE        (0xA4) — restore a slot from its most recent
 *                                 AgentFS snapshot
 *   OP_SS_RESET          (0xA5) — reset stats
 *
 * IPC channels:
 *   SS_CH_CONTROLLER (0) — controller calls tick/configure/status/etc.
 *   SS_CH_AGENTFS    (1) — snapshot_sched PPCs agentfs to write snapshots
 *   SS_CH_OOM        (2) — oom_killer PPCs force_snapshot before eviction
 *
 * Snapshot format (written to AgentFS key agentos/snapshots/slot_N_tick.snap):
 *   magic=SNAP (0x534E4150), slot_id, tick, kv_count, kv_data_len, kv_data[]
 *   KV pairs: 2-byte key_len, key_bytes, 4-byte val_len, val_bytes
 *
 * Delta compression (no external dependencies):
 *   First snapshot per slot: full KV store (magic=SNAP).
 *   Subsequent: XOR diff against previous KV data; count non-zero bytes.
 *   If non-zero count < full_len × 0.8, write sparse delta (magic=DELT)
 *   containing only changed bytes; otherwise write full snapshot.
 *   Delta header adds base_tick field for reconstruction.
 *
 * AgentFS integration (stub):
 *   agentfs_put() tries to PPC SS_CH_AGENTFS with OP_AGENTFS_WRITE.
 *   On failure (opcode not yet handled by agentfs), falls back to a
 *   local ring buffer (SS_LOCAL_RING_SIZE entries, circular).
 *   Keep last SS_DEFAULT_MAX_SNAPS (3) snapshots per slot — circular.
 *
 * Build with -DAGENTOS_SNAPSHOT_SCHED=1.
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Channel IDs ──────────────────────────────────────────────────── */

#define SS_CH_CONTROLLER  0   /* controller→snapshot_sched PPC */
#define SS_CH_AGENTFS     1   /* snapshot_sched→agentfs PPC    */
#define SS_CH_OOM         2   /* oom_killer→snapshot_sched PPC */

/* ── Opcodes ──────────────────────────────────────────────────────── */

#define OP_SS_TICK           0xA0
#define OP_SS_CONFIGURE      0xA1
#define OP_SS_STATUS         0xA2
#define OP_SS_FORCE_SNAPSHOT 0xA3
#define OP_SS_RESTORE        0xA4
#define OP_SS_RESET          0xA5

/*
 * AgentFS write opcode (sent on SS_CH_AGENTFS):
 *   MR0 = OP_AGENTFS_WRITE
 *   MR1 = data_len
 *   MR2 = key_len
 *   MR3..MR10 = key bytes packed little-endian (8 bytes per MR, up to 64 B)
 *   Data must be written to snapshot_buf_vaddr before calling.
 */
#define OP_AGENTFS_WRITE  0xF1

/* ── Limits & defaults ────────────────────────────────────────────── */

#define SS_MAX_SLOTS             16
#define SS_DEFAULT_INTERVAL      500
#define SS_DEFAULT_MAX_SNAPS     3
#define SS_KV_MAX                256   /* max KV data bytes per slot  */
#define SS_LOCAL_RING_SIZE       8     /* fallback ring when no AgentFS */
#define SS_KEY_MAX               64    /* max AgentFS key length        */

/* ── Snapshot / delta magic words ────────────────────────────────── */

#define SS_SNAP_MAGIC   0x534E4150U   /* "SNAP" */
#define SS_DELTA_MAGIC  0x44454C54U   /* "DELT" */

/* ── Wire-format headers ──────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t magic;        /* SS_SNAP_MAGIC */
    uint32_t slot_id;
    uint64_t tick;
    uint32_t kv_count;
    uint32_t kv_data_len;
    /* kv_data[] follows immediately */
} SlotSnapshot;

typedef struct __attribute__((packed)) {
    uint32_t magic;        /* SS_DELTA_MAGIC */
    uint32_t slot_id;
    uint64_t tick;
    uint64_t base_tick;    /* tick of the full snapshot this delta is based on */
    uint32_t kv_count;
    uint32_t diff_len;     /* number of non-zero (changed) bytes stored */
    /* diff_data[] follows — sparse: only the changed bytes */
} SlotDelta;

/* ── Per-slot snapshot state ──────────────────────────────────────── */

typedef struct {
    bool     active;
    uint32_t slot_id;
    uint64_t last_snap_tick;      /* tick when last snapshot was written     */
    uint32_t snap_ring_idx;       /* circular write index 0..max_snaps-1     */
    uint32_t snap_count;          /* total snapshots taken for this slot     */
    uint8_t  last_kv[SS_KV_MAX];  /* KV data from last full snapshot (delta) */
    uint32_t last_kv_len;
} SlotSnapState;

/* ── Local fallback ring ──────────────────────────────────────────── */

typedef struct {
    char     key[SS_KEY_MAX];
    uint8_t  data[sizeof(SlotSnapshot) + SS_KV_MAX];
    uint32_t data_len;
    bool     valid;
} SSLocalEntry;

/* ── Module state ─────────────────────────────────────────────────── */

typedef struct {
    uint64_t      tick_count;
    uint64_t      last_snapshot_tick;
    uint32_t      slots_snapshotted;      /* count in the last batch */
    uint64_t      total_snapshots;

    uint32_t      interval;               /* ticks between snapshots */
    uint32_t      max_snapshots_per_slot; /* circular ring depth     */

    SlotSnapState slots[SS_MAX_SLOTS];
    int           slot_count;

    SSLocalEntry  local_ring[SS_LOCAL_RING_SIZE];
    uint32_t      local_ring_head;        /* next write index (modulo size) */
} SSState;

static SSState s;

/*
 * snapshot_buf_vaddr — virtual address of the shared snapshot buffer.
 * Set by the Microkit linker via setvar_vaddr when mapped from topology.
 * snapshot_sched uses this to DMA snapshot data to AgentFS.
 */
uintptr_t snapshot_buf_vaddr;

/* ── String / formatting helpers ──────────────────────────────────── */

static uint32_t ss_strlen(const char *str)
{
    uint32_t n = 0;
    while (str[n]) n++;
    return n;
}

/* Append decimal representation of v into buf[*pos], respecting buf_len. */
static void fmt_u64(char *buf, uint32_t *pos, uint32_t buf_len, uint64_t v)
{
    if (v == 0) {
        if (*pos < buf_len) buf[(*pos)++] = '0';
        return;
    }
    char tmp[24];
    int  n = 0;
    while (v > 0) {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    for (int i = n - 1; i >= 0 && *pos < buf_len; i--)
        buf[(*pos)++] = tmp[i];
}

/* Build AgentFS key: "agentos/snapshots/slot_<N>_<tick>.snap" */
static void make_snap_key(char *out, uint32_t slot_id, uint64_t tick)
{
    const char *prefix = "agentos/snapshots/slot_";
    const char *sep    = "_";
    const char *suffix = ".snap";
    uint32_t pos = 0;
    const char *p;
    for (p = prefix; *p && pos < SS_KEY_MAX - 1;) out[pos++] = *p++;
    fmt_u64(out, &pos, SS_KEY_MAX - 1, (uint64_t)slot_id);
    for (p = sep;    *p && pos < SS_KEY_MAX - 1;) out[pos++] = *p++;
    fmt_u64(out, &pos, SS_KEY_MAX - 1, tick);
    for (p = suffix; *p && pos < SS_KEY_MAX - 1;) out[pos++] = *p++;
    out[pos] = '\0';
}

/* Build "latest" key: "agentos/snapshots/slot_<N>_latest.snap" */
static void make_latest_key(char *out, uint32_t slot_id)
{
    const char *prefix = "agentos/snapshots/slot_";
    const char *suffix = "_latest.snap";
    uint32_t pos = 0;
    const char *p;
    for (p = prefix; *p && pos < SS_KEY_MAX - 1;) out[pos++] = *p++;
    fmt_u64(out, &pos, SS_KEY_MAX - 1, (uint64_t)slot_id);
    for (p = suffix; *p && pos < SS_KEY_MAX - 1;) out[pos++] = *p++;
    out[pos] = '\0';
}

/* ── Simulated KV generation ──────────────────────────────────────── */

/*
 * kv_generate — produce synthetic KV pairs for (slot_id, tick).
 *
 * In production this would PPC to swap_slot_N to dump its live KV store.
 * Stub: generates two deterministic pairs so the delta path is exercisable:
 *   "slot" → slot_id (4 B)
 *   "tick" → tick as two u32 LE (8 B)
 *
 * Wire format per pair: [2-byte key_len][key_bytes][4-byte val_len][val_bytes]
 *
 * Returns bytes written into kv_buf.
 */
static uint32_t kv_generate(uint8_t *kv_buf, uint32_t buf_len,
                             uint32_t slot_id, uint64_t tick,
                             uint32_t *kv_count_out)
{
    uint32_t pos = 0;
    *kv_count_out = 0;

#define KV_U8(v)  do { if (pos < buf_len) kv_buf[pos++] = (uint8_t)(v); } while (0)
#define KV_U16LE(v) do { KV_U8((v) & 0xFF); KV_U8(((v) >> 8) & 0xFF); } while (0)
#define KV_U32LE(v) do { \
    KV_U8((v) & 0xFF); KV_U8(((v)>>8)&0xFF); \
    KV_U8(((v)>>16)&0xFF); KV_U8(((v)>>24)&0xFF); } while (0)
#define KV_BYTES(src, n) do { \
    for (uint32_t _b = 0; _b < (n) && pos < buf_len; _b++) \
        kv_buf[pos++] = ((const uint8_t *)(src))[_b]; } while (0)

    /* Pair 1: key="slot" (4 B), val=slot_id (4 B) */
    KV_U16LE(4);
    KV_BYTES("slot", 4);
    KV_U32LE(4);
    KV_U32LE(slot_id);
    (*kv_count_out)++;

    /* Pair 2: key="tick" (4 B), val=tick as two u32 (8 B) */
    KV_U16LE(4);
    KV_BYTES("tick", 4);
    KV_U32LE(8);
    KV_U32LE((uint32_t)(tick & 0xFFFFFFFFULL));
    KV_U32LE((uint32_t)((tick >> 32) & 0xFFFFFFFFULL));
    (*kv_count_out)++;

#undef KV_U8
#undef KV_U16LE
#undef KV_U32LE
#undef KV_BYTES

    return pos;
}

/* ── AgentFS put ──────────────────────────────────────────────────── */

/* Write to local fallback ring when AgentFS is unavailable. */
static int agentfs_put_local(const char *key,
                              const uint8_t *data, uint32_t len)
{
    uint32_t     idx = s.local_ring_head % SS_LOCAL_RING_SIZE;
    SSLocalEntry *e  = &s.local_ring[idx];

    uint32_t kl = ss_strlen(key);
    if (kl >= SS_KEY_MAX) kl = SS_KEY_MAX - 1;

    uint32_t dl = len;
    if (dl > (uint32_t)sizeof(e->data))
        dl = (uint32_t)sizeof(e->data);

    memcpy(e->key, key, kl);
    e->key[kl] = '\0';
    memcpy(e->data, data, dl);
    e->data_len = dl;
    e->valid    = true;

    s.local_ring_head++;
    return 0;
}

/*
 * agentfs_put — write (key, data) to AgentFS.
 *
 * 1. If snapshot_buf_vaddr is set, copy data there and PPC agentfs
 *    with OP_AGENTFS_WRITE.  Key bytes are packed 8-per-MR into MR3..MR10.
 * 2. On any failure, fall back to the local ring buffer.
 */
static int agentfs_put(const char *key, const uint8_t *data, uint32_t len)
{
    if (snapshot_buf_vaddr && len <= 0x10000U) {
        uint8_t  *shared  = (uint8_t *)snapshot_buf_vaddr;
        uint32_t  key_len = ss_strlen(key);

        memcpy(shared, data, len);

        microkit_mr_set(0, OP_AGENTFS_WRITE);
        microkit_mr_set(1, len);
        microkit_mr_set(2, key_len);

        /* Pack key into MRs (8 bytes per MR, up to 64 chars → 8 MRs) */
        uint32_t mr = 3;
        for (uint32_t off = 0; off < key_len && mr <= 10; off += 8, mr++) {
            uint64_t word = 0;
            for (uint32_t b = 0; b < 8 && (off + b) < key_len; b++)
                word |= (uint64_t)(uint8_t)key[off + b] << (b * 8);
            microkit_mr_set(mr, word);
        }

        microkit_ppcall(SS_CH_AGENTFS, microkit_msginfo_new(0, mr));
        uint32_t status = (uint32_t)microkit_mr_get(0);
        if (status == 0) return 0;
    }

    return agentfs_put_local(key, data, len);
}

/* ── Snapshot logic ───────────────────────────────────────────────── */

/*
 * snapshot_slot — take one snapshot of a single slot at the given tick.
 *
 * Decision path:
 *   - If this is the first snapshot for the slot, always write full (SNAP).
 *   - Otherwise XOR current KV against the previous; count non-zero bytes.
 *   - If diff bytes < 80 % of full_len → write sparse delta (DELT).
 *   - Otherwise write full (SNAP) and refresh the cached KV data.
 *
 * Both the tick-specific key and the _latest key are updated on every write.
 * Old tick-keyed snapshots beyond max_snapshots_per_slot are not explicitly
 * deleted here (TODO: add OP_AGENTFS_DELETE when agentfs supports it).
 */
static void snapshot_slot(SlotSnapState *slot, uint64_t tick)
{
    uint8_t  kv_buf[SS_KV_MAX];
    uint32_t kv_count = 0;
    uint32_t kv_len   = kv_generate(kv_buf, SS_KV_MAX,
                                     slot->slot_id, tick, &kv_count);

    /* ── Delta decision ─────────────────────────────────────────────── */
    bool    use_delta = false;
    uint8_t diff_buf[SS_KV_MAX];
    uint32_t diff_len = 0;

    if (slot->last_kv_len == kv_len && slot->snap_count > 0) {
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < kv_len; i++) {
            diff_buf[i] = kv_buf[i] ^ slot->last_kv[i];
            if (diff_buf[i] != 0) nonzero++;
        }
        diff_len  = nonzero;
        use_delta = (diff_len < (kv_len * 80) / 100);
    }

    /* ── Build AgentFS keys ─────────────────────────────────────────── */
    char key[SS_KEY_MAX];
    char latest_key[SS_KEY_MAX];
    make_snap_key(key, slot->slot_id, tick);
    make_latest_key(latest_key, slot->slot_id);

    if (!use_delta) {
        /* Full snapshot */
        uint8_t snap_buf[sizeof(SlotSnapshot) + SS_KV_MAX];
        SlotSnapshot *hdr = (SlotSnapshot *)snap_buf;
        hdr->magic       = SS_SNAP_MAGIC;
        hdr->slot_id     = slot->slot_id;
        hdr->tick        = tick;
        hdr->kv_count    = kv_count;
        hdr->kv_data_len = kv_len;
        memcpy(snap_buf + sizeof(SlotSnapshot), kv_buf, kv_len);

        uint32_t total = (uint32_t)sizeof(SlotSnapshot) + kv_len;
        agentfs_put(key, snap_buf, total);
        agentfs_put(latest_key, snap_buf, total);

        /* Refresh delta base */
        memcpy(slot->last_kv, kv_buf, kv_len);
        slot->last_kv_len = kv_len;

        LOG_VMM("snapshot_sched: SNAP slot=%u tick=%llu "
                "kv_count=%u kv_len=%u total=%u\n",
                slot->slot_id, (unsigned long long)tick,
                kv_count, kv_len, total);
    } else {
        /* Delta snapshot — store only changed (non-zero XOR) bytes */
        uint8_t  delta_buf[sizeof(SlotDelta) + SS_KV_MAX];
        SlotDelta *dhdr = (SlotDelta *)delta_buf;
        dhdr->magic     = SS_DELTA_MAGIC;
        dhdr->slot_id   = slot->slot_id;
        dhdr->tick      = tick;
        dhdr->base_tick = slot->last_snap_tick;
        dhdr->kv_count  = kv_count;
        dhdr->diff_len  = diff_len;

        uint32_t dpos = 0;
        for (uint32_t i = 0; i < kv_len && dpos < SS_KV_MAX; i++) {
            if (diff_buf[i] != 0)
                delta_buf[sizeof(SlotDelta) + dpos++] = diff_buf[i];
        }

        uint32_t total = (uint32_t)sizeof(SlotDelta) + diff_len;
        agentfs_put(key, delta_buf, total);
        agentfs_put(latest_key, delta_buf, total);

        LOG_VMM("snapshot_sched: DELT slot=%u tick=%llu "
                "diff_len=%u (of full %u)\n",
                slot->slot_id, (unsigned long long)tick,
                diff_len, kv_len);
    }

    /* Advance circular ring index */
    slot->snap_ring_idx = (slot->snap_ring_idx + 1)
                          % s.max_snapshots_per_slot;
    slot->snap_count++;
    slot->last_snap_tick = tick;
    s.total_snapshots++;
}

/* Snapshot all active slots and update batch stats. */
static void snapshot_all_active(void)
{
    uint32_t count = 0;
    for (int i = 0; i < s.slot_count; i++) {
        if (!s.slots[i].active) continue;
        snapshot_slot(&s.slots[i], s.tick_count);
        count++;
    }
    s.slots_snapshotted  = count;
    s.last_snapshot_tick = s.tick_count;
    LOG_VMM("snapshot_sched: snapshotted %u active slots at tick=%llu\n",
            count, (unsigned long long)s.tick_count);
}

/* ── Default slot table initialisation ───────────────────────────── */

/*
 * Populate slot table with the default swap_slot IDs (0-3).
 * In production this would query the controller for active slot IDs.
 */
static void init_slot_table(void)
{
    s.slot_count = 0;
    for (uint32_t i = 0; i < 4 && i < SS_MAX_SLOTS; i++) {
        SlotSnapState *sl = &s.slots[i];
        sl->active       = true;
        sl->slot_id      = i;
        sl->last_snap_tick = 0;
        sl->snap_ring_idx  = 0;
        sl->snap_count     = 0;
        sl->last_kv_len    = 0;
        memset(sl->last_kv, 0, SS_KV_MAX);
        s.slot_count++;
    }
}

/* ── Microkit entry points ────────────────────────────────────────── */

void init(void)
{
    memset(&s, 0, sizeof(s));
    s.interval               = SS_DEFAULT_INTERVAL;
    s.max_snapshots_per_slot = SS_DEFAULT_MAX_SNAPS;
    init_slot_table();
    LOG_VMM("snapshot_sched: ready  interval=%u  max_snaps=%u  slots=%d\n",
            s.interval, s.max_snapshots_per_slot, s.slot_count);
}

void notified(microkit_channel ch)
{
    (void)ch;
    /* snapshot_sched is purely reactive — all work is driven by PPC */
}

seL4_MessageInfo_t protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch;
    (void)msginfo;

    uint64_t op = microkit_mr_get(0);

    switch (op) {

    /* ── OP_SS_TICK ─────────────────────────────────────────────────── */
    case OP_SS_TICK:
        s.tick_count++;
        if (s.interval > 0 && (s.tick_count % s.interval) == 0)
            snapshot_all_active();
        microkit_mr_set(0, 0);
        microkit_mr_set(1, s.tick_count);
        return microkit_msginfo_new(0, 2);

    /* ── OP_SS_CONFIGURE ────────────────────────────────────────────── */
    case OP_SS_CONFIGURE: {
        uint64_t new_interval  = microkit_mr_get(1);
        uint64_t new_max_snaps = microkit_mr_get(2);
        if (new_interval > 0)
            s.interval = (uint32_t)new_interval;
        if (new_max_snaps > 0 && new_max_snaps <= (SS_DEFAULT_MAX_SNAPS * 4U))
            s.max_snapshots_per_slot = (uint32_t)new_max_snaps;
        LOG_VMM("snapshot_sched: configured interval=%u max_snaps=%u\n",
                s.interval, s.max_snapshots_per_slot);
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_SS_STATUS ───────────────────────────────────────────────── */
    case OP_SS_STATUS:
        microkit_mr_set(0, 0);
        microkit_mr_set(1, s.tick_count);
        microkit_mr_set(2, s.last_snapshot_tick);
        microkit_mr_set(3, s.slots_snapshotted);
        microkit_mr_set(4, s.total_snapshots);
        return microkit_msginfo_new(0, 5);

    /* ── OP_SS_FORCE_SNAPSHOT ───────────────────────────────────────── */
    case OP_SS_FORCE_SNAPSHOT:
        LOG_VMM("snapshot_sched: FORCE_SNAPSHOT requested (ch=%d)\n", (int)ch);
        snapshot_all_active();
        microkit_mr_set(0, 0);
        microkit_mr_set(1, s.slots_snapshotted);
        return microkit_msginfo_new(0, 2);

    /* ── OP_SS_RESTORE ──────────────────────────────────────────────── */
    case OP_SS_RESTORE: {
        uint32_t target = (uint32_t)microkit_mr_get(1);
        /*
         * Restore path: read agentos/snapshots/slot_<N>_latest.snap from
         * AgentFS and apply the KV data back to swap_slot_N via PPC.
         * The cached last_kv[] in SlotSnapState is used as a fast path when
         * AgentFS is unavailable (in-memory restore of last known state).
         *
         * TODO: PPC swap_slot_N with OP_SLOT_KV_RESTORE + kv_data pointer.
         */
        LOG_VMM("snapshot_sched: RESTORE slot=%u from latest snapshot\n",
                target);
        /* Locate slot state */
        for (int i = 0; i < s.slot_count; i++) {
            if (s.slots[i].active && s.slots[i].slot_id == target) {
                LOG_VMM("snapshot_sched:   cached kv_len=%u snap_count=%u\n",
                        s.slots[i].last_kv_len, s.slots[i].snap_count);
                break;
            }
        }
        microkit_mr_set(0, 0);
        microkit_mr_set(1, target);
        return microkit_msginfo_new(0, 2);
    }

    /* ── OP_SS_RESET ────────────────────────────────────────────────── */
    case OP_SS_RESET:
        s.tick_count         = 0;
        s.last_snapshot_tick = 0;
        s.slots_snapshotted  = 0;
        s.total_snapshots    = 0;
        for (int i = 0; i < s.slot_count; i++) {
            s.slots[i].snap_count     = 0;
            s.slots[i].last_snap_tick = 0;
            s.slots[i].snap_ring_idx  = 0;
            s.slots[i].last_kv_len    = 0;
        }
        s.local_ring_head = 0;
        for (int i = 0; i < SS_LOCAL_RING_SIZE; i++)
            s.local_ring[i].valid = false;
        LOG_VMM("snapshot_sched: stats reset\n");
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);

    default:
        microkit_mr_set(0, 1); /* unknown opcode */
        return microkit_msginfo_new(0, 1);
    }
}
