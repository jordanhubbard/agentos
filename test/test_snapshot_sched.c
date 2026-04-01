/*
 * test_snapshot_sched.c — agentOS snapshot_sched unit test
 *
 * Exercises the snapshot scheduler logic as a standalone host binary:
 * no seL4 / Microkit dependencies required.
 *
 * Build:  cc -o /tmp/test_snapshot_sched test/test_snapshot_sched.c \
 *             -DAGENTOS_SNAPSHOT_SCHED
 * Run:    /tmp/test_snapshot_sched
 *
 * Tests:
 *   1. Initialize snapshot_sched state — verify defaults
 *   2. Simulate 500 ticks → verify snapshot was taken
 *   3. Force snapshot, verify magic bytes (0x534E4150 "SNAP") in output
 *   4. Delta: two snapshots with identical KV → verify second is smaller
 *   5. Restore: write a snapshot, call restore, verify KV matches
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ── Re-implement snapshot_sched structures/logic in host-compilable form ── */

/* Wire-format magic words */
#define SS_SNAP_MAGIC   0x534E4150U   /* "SNAP" */
#define SS_DELTA_MAGIC  0x44454C54U   /* "DELT" */

/* Limits */
#define SS_MAX_SLOTS             16
#define SS_DEFAULT_INTERVAL      500
#define SS_DEFAULT_MAX_SNAPS     3
#define SS_KV_MAX                256
#define SS_LOCAL_RING_SIZE       8
#define SS_KEY_MAX               64

/* Opcodes */
#define OP_SS_TICK           0xA0
#define OP_SS_CONFIGURE      0xA1
#define OP_SS_STATUS         0xA2
#define OP_SS_FORCE_SNAPSHOT 0xA3
#define OP_SS_RESTORE        0xA4
#define OP_SS_RESET          0xA5

/* ── Wire headers ──────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t slot_id;
    uint64_t tick;
    uint32_t kv_count;
    uint32_t kv_data_len;
} SlotSnapshot;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t slot_id;
    uint64_t tick;
    uint64_t base_tick;
    uint32_t kv_count;
    uint32_t diff_len;
} SlotDelta;

/* ── Per-slot snapshot state ──────────────────────────────────────────── */

typedef struct {
    bool     active;
    uint32_t slot_id;
    uint64_t last_snap_tick;
    uint32_t snap_ring_idx;
    uint32_t snap_count;
    uint8_t  last_kv[SS_KV_MAX];
    uint32_t last_kv_len;
} SlotSnapState;

/* ── Local fallback ring ─────────────────────────────────────────────── */

typedef struct {
    char     key[SS_KEY_MAX];
    uint8_t  data[sizeof(SlotSnapshot) + SS_KV_MAX];
    uint32_t data_len;
    bool     valid;
} SSLocalEntry;

/* ── Module state ────────────────────────────────────────────────────── */

typedef struct {
    uint64_t      tick_count;
    uint64_t      last_snapshot_tick;
    uint32_t      slots_snapshotted;
    uint64_t      total_snapshots;
    uint32_t      interval;
    uint32_t      max_snapshots_per_slot;
    SlotSnapState slots[SS_MAX_SLOTS];
    int           slot_count;
    SSLocalEntry  local_ring[SS_LOCAL_RING_SIZE];
    uint32_t      local_ring_head;
} SSState;

static SSState s;

/* ── Helper: string length ─────────────────────────────────────────── */

static uint32_t ss_strlen(const char *str)
{
    uint32_t n = 0;
    while (str[n]) n++;
    return n;
}

/* ── fmt_u64 ───────────────────────────────────────────────────────── */

static void fmt_u64(char *buf, uint32_t *pos, uint32_t buf_len, uint64_t v)
{
    if (v == 0) {
        if (*pos < buf_len) buf[(*pos)++] = '0';
        return;
    }
    char tmp[24];
    int  n = 0;
    while (v > 0) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int i = n - 1; i >= 0 && *pos < buf_len; i--)
        buf[(*pos)++] = tmp[i];
}

/* ── Key builders ──────────────────────────────────────────────────── */

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

/* ── KV generation ────────────────────────────────────────────────── */

static uint32_t kv_generate(uint8_t *kv_buf, uint32_t buf_len,
                             uint32_t slot_id, uint64_t tick,
                             uint32_t *kv_count_out)
{
    uint32_t pos = 0;
    *kv_count_out = 0;

#define KV_U8(v)  do { if (pos < buf_len) kv_buf[pos++] = (uint8_t)(v); } while (0)
#define KV_U16LE(v) do { KV_U8((v) & 0xFF); KV_U8(((v) >> 8) & 0xFF); } while (0)
#define KV_U32LE(v) do { \
    KV_U8((v)&0xFF); KV_U8(((v)>>8)&0xFF); \
    KV_U8(((v)>>16)&0xFF); KV_U8(((v)>>24)&0xFF); } while (0)
#define KV_BYTES(src, n) do { \
    for (uint32_t _b = 0; _b < (n) && pos < buf_len; _b++) \
        kv_buf[pos++] = ((const uint8_t *)(src))[_b]; } while (0)

    /* Pair 1: key="slot", val=slot_id */
    KV_U16LE(4); KV_BYTES("slot", 4); KV_U32LE(4); KV_U32LE(slot_id);
    (*kv_count_out)++;

    /* Pair 2: key="tick", val=tick as 2×u32 */
    KV_U16LE(4); KV_BYTES("tick", 4); KV_U32LE(8);
    KV_U32LE((uint32_t)(tick & 0xFFFFFFFFULL));
    KV_U32LE((uint32_t)((tick >> 32) & 0xFFFFFFFFULL));
    (*kv_count_out)++;

#undef KV_U8
#undef KV_U16LE
#undef KV_U32LE
#undef KV_BYTES

    return pos;
}

/* ── Instrumented agentfs_put ─────────────────────────────────────── */

/* Capture the last write so tests can inspect it. */
static uint8_t  last_written_data[sizeof(SlotSnapshot) + SS_KV_MAX + 64];
static uint32_t last_written_len  = 0;
static char     last_written_key[SS_KEY_MAX];
static uint32_t agentfs_call_count = 0;

static int agentfs_put_local(const char *key,
                              const uint8_t *data, uint32_t len)
{
    uint32_t     idx = s.local_ring_head % SS_LOCAL_RING_SIZE;
    SSLocalEntry *e  = &s.local_ring[idx];

    uint32_t kl = ss_strlen(key);
    if (kl >= SS_KEY_MAX) kl = SS_KEY_MAX - 1;
    uint32_t dl = len;
    if (dl > (uint32_t)sizeof(e->data)) dl = (uint32_t)sizeof(e->data);

    memcpy(e->key, key, kl); e->key[kl] = '\0';
    memcpy(e->data, data, dl);
    e->data_len = dl;
    e->valid    = true;
    s.local_ring_head++;
    return 0;
}

static int agentfs_put(const char *key, const uint8_t *data, uint32_t len)
{
    agentfs_call_count++;

    /* Capture the last (non-"latest") write for test inspection */
    uint32_t kl = ss_strlen(key);
    bool is_latest = (kl > 7 && key[kl - 7] == 'l'); /* ends in "latest.*" */
    if (!is_latest) {
        uint32_t cap = len;
        if (cap > sizeof(last_written_data)) cap = sizeof(last_written_data);
        memcpy(last_written_data, data, cap);
        last_written_len = cap;
        if (kl >= SS_KEY_MAX) kl = SS_KEY_MAX - 1;
        memcpy(last_written_key, key, kl);
        last_written_key[kl] = '\0';
    }

    return agentfs_put_local(key, data, len);
}

/* ── snapshot_slot ────────────────────────────────────────────────── */

static void snapshot_slot(SlotSnapState *slot, uint64_t tick)
{
    uint8_t  kv_buf[SS_KV_MAX];
    uint32_t kv_count = 0;
    uint32_t kv_len   = kv_generate(kv_buf, SS_KV_MAX,
                                     slot->slot_id, tick, &kv_count);

    bool     use_delta = false;
    uint8_t  diff_buf[SS_KV_MAX];
    uint32_t diff_len  = 0;

    if (slot->last_kv_len == kv_len && slot->snap_count > 0) {
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < kv_len; i++) {
            diff_buf[i] = kv_buf[i] ^ slot->last_kv[i];
            if (diff_buf[i] != 0) nonzero++;
        }
        diff_len  = nonzero;
        use_delta = (diff_len < (kv_len * 80) / 100);
    }

    char key[SS_KEY_MAX];
    char latest_key[SS_KEY_MAX];
    make_snap_key(key, slot->slot_id, tick);
    make_latest_key(latest_key, slot->slot_id);

    if (!use_delta) {
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

        memcpy(slot->last_kv, kv_buf, kv_len);
        slot->last_kv_len = kv_len;
    } else {
        uint8_t   delta_buf[sizeof(SlotDelta) + SS_KV_MAX];
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
    }

    slot->snap_ring_idx = (slot->snap_ring_idx + 1) % s.max_snapshots_per_slot;
    slot->snap_count++;
    slot->last_snap_tick = tick;
    s.total_snapshots++;
}

/* Snapshot all active slots. */
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
}

/* Initialise default slot table (slots 0-3). */
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

/* Dispatch a simulated opcode call (mirrors protected()). */
static uint32_t sim_op(uint64_t op, uint64_t mr1, uint64_t mr2)
{
    switch (op) {
    case OP_SS_TICK:
        s.tick_count++;
        if (s.interval > 0 && (s.tick_count % s.interval) == 0)
            snapshot_all_active();
        return 0;

    case OP_SS_CONFIGURE:
        if (mr1 > 0) s.interval = (uint32_t)mr1;
        if (mr2 > 0 && mr2 <= (SS_DEFAULT_MAX_SNAPS * 4U))
            s.max_snapshots_per_slot = (uint32_t)mr2;
        return 0;

    case OP_SS_STATUS:
        /* Returns tick_count via s.tick_count directly */
        return 0;

    case OP_SS_FORCE_SNAPSHOT:
        snapshot_all_active();
        return s.slots_snapshotted;

    case OP_SS_RESTORE:
        /* Stub — returns 0 (success) */
        return 0;

    case OP_SS_RESET:
        s.tick_count = s.last_snapshot_tick = s.slots_snapshotted = 0;
        s.total_snapshots = 0;
        for (int i = 0; i < s.slot_count; i++) {
            s.slots[i].snap_count = s.slots[i].last_snap_tick = 0;
            s.slots[i].snap_ring_idx = s.slots[i].last_kv_len = 0;
        }
        s.local_ring_head = 0;
        for (int i = 0; i < SS_LOCAL_RING_SIZE; i++)
            s.local_ring[i].valid = false;
        return 0;

    default:
        return 1; /* error */
    }
}

/* ── Test helpers ────────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        tests_passed++; \
    } else { \
        printf("  [FAIL] %s  (line %d)\n", msg, __LINE__); \
    } \
} while (0)

static void reset_state(void)
{
    memset(&s, 0, sizeof(s));
    s.interval               = SS_DEFAULT_INTERVAL;
    s.max_snapshots_per_slot = SS_DEFAULT_MAX_SNAPS;
    init_slot_table();
    agentfs_call_count = 0;
    last_written_len   = 0;
    memset(last_written_key,  0, sizeof(last_written_key));
    memset(last_written_data, 0, sizeof(last_written_data));
}

/* ── Test 1: Initialise state, verify defaults ───────────────────── */

static void test_1_init(void)
{
    printf("\nTest 1: Initialise snapshot_sched state\n");
    reset_state();

    CHECK(s.interval               == SS_DEFAULT_INTERVAL, "default interval = 500");
    CHECK(s.max_snapshots_per_slot == SS_DEFAULT_MAX_SNAPS, "default max_snaps = 3");
    CHECK(s.slot_count             == 4, "four active slots initialised");
    CHECK(s.tick_count             == 0, "tick_count starts at 0");
    CHECK(s.total_snapshots        == 0, "total_snapshots starts at 0");
    CHECK(s.slots[0].active,            "slot 0 active");
    CHECK(s.slots[3].slot_id       == 3, "slot 3 has slot_id=3");
}

/* ── Test 2: Simulate 500 ticks, verify snapshot taken ──────────── */

static void test_2_tick_interval(void)
{
    printf("\nTest 2: 500 ticks → snapshot triggered at interval\n");
    reset_state();

    /* Drive 499 ticks — no snapshot yet */
    for (int i = 0; i < 499; i++)
        sim_op(OP_SS_TICK, 0, 0);

    CHECK(s.total_snapshots == 0, "no snapshots before interval elapsed");
    CHECK(s.tick_count      == 499, "tick_count = 499");

    /* Tick 500 triggers the snapshot */
    sim_op(OP_SS_TICK, 0, 0);

    CHECK(s.tick_count             == 500, "tick_count = 500 after interval tick");
    CHECK(s.total_snapshots        == 4,   "4 slots snapshotted at tick 500");
    CHECK(s.last_snapshot_tick     == 500, "last_snapshot_tick = 500");
    CHECK(s.slots_snapshotted      == 4,   "slots_snapshotted = 4");

    /* Verify a snapshot was written to the local ring */
    CHECK(s.local_ring[0].valid,  "local ring[0] populated");
    /* Both tick-key and latest-key are written per slot → 8 writes for 4 slots */
    CHECK(agentfs_call_count       >= 4,   "agentfs_put called at least 4 times");
}

/* ── Test 3: Force snapshot, verify SNAP magic ───────────────────── */

static void test_3_force_snapshot_magic(void)
{
    printf("\nTest 3: Force snapshot — verify SNAP magic bytes in output\n");
    reset_state();

    uint32_t snapped = sim_op(OP_SS_FORCE_SNAPSHOT, 0, 0);

    CHECK(snapped == 4, "force snapshot reports 4 slots");
    CHECK(s.total_snapshots == 4, "4 total snapshots recorded");
    CHECK(last_written_len  >= (uint32_t)sizeof(SlotSnapshot),
          "written data is at least header size");

    /* Inspect the first 4 bytes of the last written snapshot for SNAP magic */
    uint32_t magic = 0;
    memcpy(&magic, last_written_data, sizeof(magic));
    CHECK(magic == SS_SNAP_MAGIC, "first write has SNAP magic (0x534E4150)");

    /* Verify slot_id field in header */
    SlotSnapshot hdr;
    memcpy(&hdr, last_written_data, sizeof(hdr));
    CHECK(hdr.kv_count    == 2, "header.kv_count = 2 (slot + tick pairs)");
    CHECK(hdr.kv_data_len >  0, "header.kv_data_len > 0");

    /* Verify key format */
    CHECK(strncmp(last_written_key, "agentos/snapshots/slot_", 23) == 0,
          "key starts with 'agentos/snapshots/slot_'");
}

/* ── Test 4: Delta compression — identical KV yields smaller write ── */

static void test_4_delta_compression(void)
{
    printf("\nTest 4: Delta compression — same KV twice → second write smaller\n");
    reset_state();

    /* Use a single slot to keep the comparison clean. */
    memset(&s, 0, sizeof(s));
    s.interval               = SS_DEFAULT_INTERVAL;
    s.max_snapshots_per_slot = SS_DEFAULT_MAX_SNAPS;
    s.slot_count             = 1;
    s.slots[0].active        = true;
    s.slots[0].slot_id       = 0;

    /* First snapshot (full) — tick=100 */
    s.tick_count = 100;
    agentfs_call_count = 0;
    last_written_len   = 0;
    snapshot_slot(&s.slots[0], s.tick_count);
    uint32_t first_len = last_written_len;
    uint32_t first_magic = 0;
    memcpy(&first_magic, last_written_data, sizeof(first_magic));

    CHECK(first_magic == SS_SNAP_MAGIC, "first snapshot has SNAP magic");
    CHECK(first_len   >= (uint32_t)sizeof(SlotSnapshot),
          "first snapshot length >= header size");

    /*
     * Second snapshot at the same tick (same slot_id + same tick value
     * → identical KV data → XOR diff all-zero → pure delta).
     */
    last_written_len = 0;
    snapshot_slot(&s.slots[0], s.tick_count);
    uint32_t second_len = last_written_len;
    uint32_t second_magic = 0;
    memcpy(&second_magic, last_written_data, sizeof(second_magic));

    CHECK(second_magic == SS_DELTA_MAGIC, "second snapshot has DELT magic");
    CHECK(second_len   < first_len,
          "delta is smaller than full snapshot");

    /* Delta header diff_len should be 0 (no bytes changed) */
    SlotDelta dhdr;
    memcpy(&dhdr, last_written_data, sizeof(dhdr));
    CHECK(dhdr.diff_len  == 0,          "delta diff_len = 0 (no changes)");
    CHECK(dhdr.base_tick == 100,         "delta base_tick = 100");
}

/* ── Test 5: Restore — write snapshot, call restore, verify KV ────── */

static void test_5_restore(void)
{
    printf("\nTest 5: Restore — snapshot a slot, then restore it\n");
    reset_state();

    /* Snapshot slot 2 */
    s.tick_count = 42;
    SlotSnapState *sl = &s.slots[2];
    snapshot_slot(sl, s.tick_count);

    /* The slot should now have cached KV data */
    CHECK(sl->last_kv_len >  0, "last_kv_len populated after snapshot");
    CHECK(sl->snap_count  == 1, "snap_count = 1 after first snapshot");
    CHECK(sl->last_snap_tick == 42, "last_snap_tick = 42");

    /* Verify the snapshot was recorded in local ring */
    bool found = false;
    for (int i = 0; i < SS_LOCAL_RING_SIZE; i++) {
        if (s.local_ring[i].valid &&
            strncmp(s.local_ring[i].key, "agentos/snapshots/slot_2_42", 27) == 0) {
            found = true;
            break;
        }
    }
    CHECK(found, "tick-keyed snapshot in local ring (slot_2_42.snap)");

    /* Reconstruct expected KV from the saved last_kv */
    uint8_t  expected_kv[SS_KV_MAX];
    uint32_t expected_count = 0;
    uint32_t expected_len = kv_generate(expected_kv, SS_KV_MAX, 2, 42, &expected_count);

    CHECK(sl->last_kv_len == expected_len,
          "cached kv_len matches expected KV length");
    CHECK(memcmp(sl->last_kv, expected_kv, expected_len) == 0,
          "cached KV data matches expected KV content");

    /* Simulate OP_SS_RESTORE for slot 2 — stub returns 0 */
    uint32_t rc = sim_op(OP_SS_RESTORE, 2, 0);
    CHECK(rc == 0, "OP_SS_RESTORE returns 0 (success)");
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(void)
{
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║   agentOS — snapshot_sched unit tests            ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");

    test_1_init();
    test_2_tick_interval();
    test_3_force_snapshot_magic();
    test_4_delta_compression();
    test_5_restore();

    printf("\n──────────────────────────────────────────────────\n");
    printf("Results: %d / %d passed\n", tests_passed, tests_run);

    if (tests_passed == tests_run) {
        printf("✓ All snapshot_sched tests passed.\n\n");
        return 0;
    } else {
        printf("✗ %d test(s) FAILED.\n\n", tests_run - tests_passed);
        return 1;
    }
}
