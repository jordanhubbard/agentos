/*
 * agentOS VibeOS Contract Test Suite
 *
 * Comprehensive contract tests for the VibeOS lifecycle API:
 *
 *   (1) Non-reinvention: MSG_VIBEOS_BIND_DEVICE for a device class already
 *       served by a ring-0 PD returns the EXISTING handle; no second PD is
 *       registered.
 *
 *   (2) Non-escalation: MSG_VIBEOS_BIND_DEVICE with a ring-0 channel ID
 *       passed directly as dev_handle must be rejected (VIBEOS_ERR_BIND_FAIL).
 *
 *   (3) WASM load + boot + snapshot + restore round-trip: module state
 *       (context state, device bindings) survives snapshot/restore.
 *
 *   (4) Migrate: snapshot + destroy + restore on a new context — device
 *       handles reconnect (device_flags and dev_handles preserved).
 *
 *   (5) Double-boot: MSG_VIBEOS_BOOT on an already-booted context returns
 *       VIBEOS_ERR_BAD_STATE (EALREADY equivalent).
 *
 * Build:
 *   cc -o /tmp/test_vibeos_contract \
 *       tests/vibe/test_vibeos_contract.c \
 *       -I tests \
 *       -I kernel/agentos-root-task/include \
 *       -DAGENTOS_TEST_HOST
 * Run:
 *   /tmp/test_vibeos_contract
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ─── Host stubs ─────────────────────────────────────────────────────────── */

static uint64_t _stub_mrs[64];
static inline void     microkit_mr_set(uint32_t i, uint64_t v) { _stub_mrs[i] = v; }
static inline uint64_t microkit_mr_get(uint32_t i)             { return _stub_mrs[i]; }
static inline void microkit_notify(uint32_t ch) { (void)ch; }
static inline void microkit_dbg_puts(const char *s) { (void)s; }

typedef uint64_t microkit_msginfo;
static inline microkit_msginfo microkit_msginfo_new(uint64_t label, uint32_t count) {
    (void)count; return label;
}
static void log_drain_write(int ch, int tag, const char *s) { (void)ch; (void)tag; (void)s; }

/* Stub staging region (4MB; matches the live VibeEngine layout) */
static uint8_t _stub_staging[0x400000];
static uintptr_t vibe_staging_vaddr;

static inline void agentos_wmb(void) {}

/* ─── VibeOS constants (mirrors vibeos_contract.h) ───────────────────────── */

#define VIBEOS_TYPE_LINUX        0x01u
#define VIBEOS_TYPE_FREEBSD      0x02u
#define VIBEOS_ARCH_AARCH64      0x01u
#define VIBEOS_ARCH_X86_64       0x02u

#define VIBEOS_DEV_SERIAL        (1u << 0)
#define VIBEOS_DEV_NET           (1u << 1)
#define VIBEOS_DEV_BLOCK         (1u << 2)
#define VIBEOS_DEV_USB           (1u << 3)
#define VIBEOS_DEV_FB            (1u << 4)

#define VIBEOS_FUNC_CLASS_SERIAL 0x01u
#define VIBEOS_FUNC_CLASS_NET    0x02u
#define VIBEOS_FUNC_CLASS_BLOCK  0x03u
#define VIBEOS_FUNC_CLASS_USB    0x04u
#define VIBEOS_FUNC_CLASS_FB     0x05u

#define VIBEOS_MODULE_TYPE_WASM  1u
#define VIBEOS_MODULE_TYPE_ELF   2u

#define VIBEOS_STATE_CREATING    0
#define VIBEOS_STATE_BOOTING     1
#define VIBEOS_STATE_RUNNING     2
#define VIBEOS_STATE_PAUSED      3
#define VIBEOS_STATE_DEAD        4

#define VIBEOS_OK                     0
#define VIBEOS_ERR_NO_SLOTS           1
#define VIBEOS_ERR_BAD_HANDLE         2
#define VIBEOS_ERR_BAD_OS_TYPE        3
#define VIBEOS_ERR_DEVICE_UNAVAILABLE 4
#define VIBEOS_ERR_BIND_FAIL          5
#define VIBEOS_ERR_WASM_LOAD_FAIL     6
#define VIBEOS_ERR_BAD_MODULE_TYPE    9
#define VIBEOS_ERR_BAD_STATE          10
#define VIBEOS_ERR_BAD_FUNC_CLASS     11

#define CAP_POLICY_FUNC_CLASS_MAX     0x05u
#define STAGING_SIZE                  0x400000UL

/* Known ring-0 system channels (subset; mirrors cap_policy.c / agentos.h).
 * Guest VMMs and untrusted callers must NOT pass these as dev_handle in
 * MSG_VIBEOS_BIND_DEVICE — doing so is a privilege escalation attempt. */
static const uint32_t g_ring0_channels[] = {
     5u,  /* CH_AGENTFS */
     6u,  /* CH_TRACE_CTRL */
    18u,  /* CH_NAMESERVER */
    19u,  /* CH_VFS_SERVER */
    20u,  /* CH_SPAWN_SERVER */
    21u,  /* CH_NET_SERVER */
    22u,  /* CH_VIRTIO_BLK */
    23u,  /* CH_APP_MANAGER */
    24u,  /* CH_HTTP_SVC */
    40u,  /* CH_VIBEENGINE */
    45u,  /* CH_VM_MANAGER */
    52u,  /* CH_QUOTA_CTRL */
    56u,  /* CH_WATCHDOG_CTRL */
    57u,  /* CH_CAP_AUDIT_CTRL */
};
#define RING0_CH_N \
    ((uint32_t)(sizeof(g_ring0_channels) / sizeof(g_ring0_channels[0])))

/* Returns 1 if ch is a known ring-0 system channel, 0 otherwise. */
static int cap_policy_is_ring0_channel(uint32_t ch) {
    for (uint32_t i = 0; i < RING0_CH_N; i++) {
        if (g_ring0_channels[i] == ch) return 1;
    }
    return 0;
}

/* ─── Ring-0 service registry (non-reinvention; mirrors cap_policy.c) ────── */

typedef struct {
    uint32_t pd_handle;
    uint32_t channel_id;
    bool     registered;
} ring0_svc_entry_t;

static ring0_svc_entry_t g_ring0_registry[CAP_POLICY_FUNC_CLASS_MAX + 1];

static int cap_policy_find_ring0_service(uint32_t fc, uint32_t *pd, uint32_t *ch) {
    if (fc < 1 || fc > CAP_POLICY_FUNC_CLASS_MAX) return 0;
    if (!g_ring0_registry[fc].registered)          return 0;
    if (pd) *pd = g_ring0_registry[fc].pd_handle;
    if (ch) *ch = g_ring0_registry[fc].channel_id;
    return 1;
}
static int cap_policy_register_ring0_service(uint32_t fc, uint32_t pd, uint32_t ch) {
    if (fc < 1 || fc > CAP_POLICY_FUNC_CLASS_MAX) return -1;
    if (g_ring0_registry[fc].registered)           return -1;
    g_ring0_registry[fc].pd_handle  = pd;
    g_ring0_registry[fc].channel_id = ch;
    g_ring0_registry[fc].registered = true;
    return 0;
}
/* ─── VibeOS context table (mirrors vibe_engine.c) ───────────────────────── */

#define MAX_VIBEOS_INSTANCES  4
#define VIBEOS_DEV_COUNT      5

typedef struct {
    uint32_t handle;
    uint32_t os_type;
    uint32_t arch;
    uint32_t state;
    uint32_t ram_mb;
    uint32_t cpu_budget_us;
    uint32_t cpu_period_us;
    uint32_t device_flags;
    uint32_t dev_handles[VIBEOS_DEV_COUNT];
    uint32_t dev_channels[VIBEOS_DEV_COUNT];
    uint32_t uptime_ticks;
} vibeos_ctx_t;

static vibeos_ctx_t g_vibeos[MAX_VIBEOS_INSTANCES];
static uint32_t     g_next_vibeos_handle = 1;

static vibeos_ctx_t *vibeos_find(uint32_t h) {
    for (int i = 0; i < MAX_VIBEOS_INSTANCES; i++)
        if (g_vibeos[i].handle == h) return &g_vibeos[i];
    return (vibeos_ctx_t *)0;
}
static vibeos_ctx_t *vibeos_alloc(void) {
    for (int i = 0; i < MAX_VIBEOS_INSTANCES; i++)
        if (g_vibeos[i].handle == 0) return &g_vibeos[i];
    return (vibeos_ctx_t *)0;
}
static uint32_t dev_type_to_func_class(uint32_t dt) { return dt + 1; }

static bool validate_wasm_header(const uint8_t *d, uint32_t sz) {
    if (sz < 8) return false;
    return d[0]==0x00 && d[1]==0x61 && d[2]==0x73 && d[3]==0x6D;
}

static uint32_t next_proposal_id = 1;

/* ─── Snapshot store ─────────────────────────────────────────────────────── */

#define MAX_SNAPSHOTS 8

static vibeos_ctx_t g_snapshots[MAX_SNAPSHOTS];
static bool         g_snap_used[MAX_SNAPSHOTS];

/* ─── VibeOS IPC handlers ────────────────────────────────────────────────── */

/* MSG_VIBEOS_CREATE */
static microkit_msginfo handle_vibeos_create(void)
{
    struct __attribute__((packed)) vibeos_create_req {
        uint8_t  os_type;
        uint8_t  arch;
        uint8_t  _pad[2];
        uint32_t ram_mb;
        uint32_t cpu_budget_us;
        uint32_t cpu_period_us;
        uint32_t device_flags;
        uint8_t  wasm_hash[32];
    };
    const struct vibeos_create_req *req =
        (const struct vibeos_create_req *)vibe_staging_vaddr;

    vibeos_ctx_t *ctx = vibeos_alloc();
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_NO_SLOTS);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }
    if (req->os_type != VIBEOS_TYPE_LINUX && req->os_type != VIBEOS_TYPE_FREEBSD) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_OS_TYPE);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    uint32_t h = g_next_vibeos_handle++;
    if (g_next_vibeos_handle == 0) g_next_vibeos_handle = 1;

    ctx->handle        = h;
    ctx->os_type       = req->os_type;
    ctx->arch          = req->arch;
    ctx->state         = VIBEOS_STATE_CREATING;
    ctx->ram_mb        = req->ram_mb;
    ctx->cpu_budget_us = req->cpu_budget_us;
    ctx->cpu_period_us = req->cpu_period_us;
    ctx->device_flags  = 0;
    ctx->uptime_ticks  = 0;
    for (int i = 0; i < VIBEOS_DEV_COUNT; i++) {
        ctx->dev_handles[i]  = 0;
        ctx->dev_channels[i] = 0;
    }

    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, h);
    return microkit_msginfo_new(0, 2);
}

/*
 * MSG_VIBEOS_BIND_DEVICE
 *
 * Non-reinvention: if a ring-0 PD already serves this function class, the
 * caller's new dev_handle is rejected and the existing handle is returned.
 *
 * Non-escalation: if the caller passes a ring-0 channel ID directly as
 * dev_handle (privilege escalation attempt), the request is denied with
 * VIBEOS_ERR_BIND_FAIL.
 */
static microkit_msginfo handle_vibeos_bind_device(void)
{
    uint32_t handle     = (uint32_t)microkit_mr_get(1);
    uint32_t dev_type   = (uint32_t)microkit_mr_get(2);
    uint32_t dev_handle = (uint32_t)microkit_mr_get(3);

    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0);
        microkit_mr_set(2, 0);
        return microkit_msginfo_new(0, 3);
    }
    if (dev_type >= VIBEOS_DEV_COUNT) {
        microkit_mr_set(0, VIBEOS_ERR_DEVICE_UNAVAILABLE);
        microkit_mr_set(1, 0);
        microkit_mr_set(2, 0);
        return microkit_msginfo_new(0, 3);
    }

    /* Non-escalation: reject ring-0 channel IDs passed as device handles */
    if (cap_policy_is_ring0_channel(dev_handle)) {
        microkit_mr_set(0, VIBEOS_ERR_BIND_FAIL);
        microkit_mr_set(1, 0);
        microkit_mr_set(2, 0);
        return microkit_msginfo_new(0, 3);
    }

    uint32_t fc = dev_type_to_func_class(dev_type);
    uint32_t ex_pd = 0, ex_ch = 0, preexisting = 0, effective_h = dev_handle;

    /* Non-reinvention: check for existing ring-0 service */
    if (cap_policy_find_ring0_service(fc, &ex_pd, &ex_ch)) {
        effective_h = ex_pd;
        preexisting = 1;
        ctx->dev_handles[dev_type]  = ex_pd;
        ctx->dev_channels[dev_type] = ex_ch;
        ctx->device_flags |= (1u << dev_type);
    } else {
        cap_policy_register_ring0_service(fc, dev_handle, 0);
        ctx->dev_handles[dev_type]  = dev_handle;
        ctx->dev_channels[dev_type] = 0;
        ctx->device_flags |= (1u << dev_type);
    }

    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, effective_h);
    microkit_mr_set(2, preexisting);
    return microkit_msginfo_new(0, 3);
}

/* MSG_VIBEOS_BOOT */
static microkit_msginfo handle_vibeos_boot(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (ctx->state != VIBEOS_STATE_CREATING) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_STATE);
        return microkit_msginfo_new(0, 1);
    }
    ctx->state = VIBEOS_STATE_BOOTING;
    microkit_mr_set(0, VIBEOS_OK);
    return microkit_msginfo_new(0, 1);
}

/* MSG_VIBEOS_LOAD_MODULE */
static microkit_msginfo handle_vibeos_load_module(void)
{
    uint32_t handle      = (uint32_t)microkit_mr_get(1);
    uint32_t module_type = (uint32_t)microkit_mr_get(2);
    uint32_t module_size = (uint32_t)microkit_mr_get(3);

    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }
    if (module_type != VIBEOS_MODULE_TYPE_WASM &&
        module_type != VIBEOS_MODULE_TYPE_ELF) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_MODULE_TYPE);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }
    if (module_type == VIBEOS_MODULE_TYPE_WASM) {
        if (!validate_wasm_header((const uint8_t *)vibe_staging_vaddr, module_size)) {
            microkit_mr_set(0, VIBEOS_ERR_WASM_LOAD_FAIL);
            microkit_mr_set(1, 0);
            return microkit_msginfo_new(0, 2);
        }
    }

    uint32_t swap_id = next_proposal_id++;
    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, swap_id);
    return microkit_msginfo_new(0, 2);
}

/*
 * MSG_VIBEOS_SNAPSHOT
 *
 * Saves the vibeos_ctx_t to an in-memory snapshot slot.
 * Returns snap_lo = slot index (snap_hi = 0 in this stub).
 * In the production implementation this checkpoint is written to AgentFS.
 */
static microkit_msginfo handle_vibeos_snapshot(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0);
        microkit_mr_set(2, 0);
        return microkit_msginfo_new(0, 3);
    }

    int idx = -1;
    for (int i = 0; i < MAX_SNAPSHOTS; i++) {
        if (!g_snap_used[i]) { idx = i; break; }
    }
    if (idx < 0) {
        microkit_mr_set(0, VIBEOS_ERR_NO_SLOTS);
        microkit_mr_set(1, 0);
        microkit_mr_set(2, 0);
        return microkit_msginfo_new(0, 3);
    }

    g_snapshots[idx] = *ctx;
    g_snap_used[idx] = true;

    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, (uint32_t)idx);  /* snap_lo = slot index */
    microkit_mr_set(2, 0u);             /* snap_hi = 0 in stub */
    return microkit_msginfo_new(0, 3);
}

/*
 * MSG_VIBEOS_RESTORE
 *
 * Restores context fields from the snapshot identified by (snap_lo, snap_hi).
 * The context's handle is preserved; everything else is overwritten.
 */
static microkit_msginfo handle_vibeos_restore(void)
{
    uint32_t handle  = (uint32_t)microkit_mr_get(1);
    uint32_t snap_lo = (uint32_t)microkit_mr_get(2);
    /* snap_hi unused in stub */

    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (snap_lo >= MAX_SNAPSHOTS || !g_snap_used[snap_lo]) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    uint32_t saved_handle = ctx->handle;
    *ctx = g_snapshots[snap_lo];
    ctx->handle = saved_handle;  /* restore to destination context slot */

    microkit_mr_set(0, VIBEOS_OK);
    return microkit_msginfo_new(0, 1);
}

/*
 * MSG_VIBEOS_DESTROY
 *
 * Releases the vibeos_ctx_t slot. Sets handle to 0 to mark the slot free.
 */
static microkit_msginfo handle_vibeos_destroy(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    memset(ctx, 0, sizeof(*ctx));  /* ctx->handle = 0 marks slot free */
    microkit_mr_set(0, VIBEOS_OK);
    return microkit_msginfo_new(0, 1);
}

/* ─── Test helpers ───────────────────────────────────────────────────────── */

static void reset_state(void) {
    memset(g_vibeos,       0, sizeof(g_vibeos));
    memset(g_ring0_registry, 0, sizeof(g_ring0_registry));
    memset(g_snapshots,    0, sizeof(g_snapshots));
    memset(g_snap_used,    0, sizeof(g_snap_used));
    memset(_stub_staging,  0, sizeof(_stub_staging));
    g_next_vibeos_handle = 1;
    next_proposal_id     = 1;
    vibe_staging_vaddr   = (uintptr_t)_stub_staging;
}

static uint32_t do_create(uint8_t os_type, uint8_t arch, uint32_t ram_mb) {
    struct __attribute__((packed)) {
        uint8_t  os_type;
        uint8_t  arch;
        uint8_t  _pad[2];
        uint32_t ram_mb;
        uint32_t cpu_budget_us;
        uint32_t cpu_period_us;
        uint32_t device_flags;
        uint8_t  wasm_hash[32];
    } *req = (void *)vibe_staging_vaddr;
    req->os_type = os_type;
    req->arch    = arch;
    req->ram_mb  = ram_mb;
    handle_vibeos_create();
    return (uint32_t)_stub_mrs[1];
}

static void write_valid_wasm_magic(void) {
    uint8_t *s = _stub_staging;
    s[0] = 0x00; s[1] = 0x61; s[2] = 0x73; s[3] = 0x6D;
    s[4] = 0x01; s[5] = 0x00; s[6] = 0x00; s[7] = 0x00;
}

/* ─── Test framework ─────────────────────────────────────────────────────── */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("\n=== TEST: %s ===\n", (name))
#define ASSERT_EQ(a, b, msg) do { \
    if ((int64_t)(a) != (int64_t)(b)) { \
        printf("  FAIL: %s (got %lld expected %lld)\n", (msg), \
               (long long)(a), (long long)(b)); \
        tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)
#define ASSERT_NE(a, b, msg) do { \
    if ((int64_t)(a) == (int64_t)(b)) { \
        printf("  FAIL: %s (got %lld, expected != %lld)\n", (msg), \
               (long long)(a), (long long)(b)); \
        tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

/* ════════════════════════════════════════════════════════════════════════════
 * Test Group 1: Non-reinvention
 *
 * Invariant: at most one ring-0 PD per device function class.
 * When a second BIND_DEVICE is issued for the same class, the EXISTING
 * handle must be returned and no second PD is registered.
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_non_reinvention_returns_existing_handle(void)
{
    TEST("non_reinvention: second bind returns existing handle");
    reset_state();

    /* OS-A: bind serial PD with handle 111 */
    uint32_t hA = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);
    microkit_mr_set(1, hA);
    microkit_mr_set(2, 0);   /* dev_type 0 = SERIAL */
    microkit_mr_set(3, 111); /* first PD handle */
    handle_vibeos_bind_device();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "OS-A bind serial: ok");
    ASSERT_EQ(_stub_mrs[2], 0u,        "OS-A bind serial: preexisting=0 (new)");

    /* OS-B: attempts to bind a DIFFERENT serial PD with handle 222 */
    uint32_t hB = do_create(VIBEOS_TYPE_FREEBSD, VIBEOS_ARCH_X86_64, 128);
    microkit_mr_set(1, hB);
    microkit_mr_set(2, 0);   /* dev_type 0 = SERIAL */
    microkit_mr_set(3, 222); /* second (new) PD handle — must be rejected */
    handle_vibeos_bind_device();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK,  "OS-B bind serial: ok (not error)");
    ASSERT_EQ(_stub_mrs[1], 111u, "OS-B effective_handle = existing 111 (not 222)");
    ASSERT_EQ(_stub_mrs[2], 1u,   "OS-B preexisting = 1 (reuse forced)");
}

static void test_non_reinvention_no_second_pd_created(void)
{
    TEST("non_reinvention: cap_policy registry unchanged after forced reuse");
    reset_state();

    uint32_t hA = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);
    microkit_mr_set(1, hA); microkit_mr_set(2, 1); microkit_mr_set(3, 300);
    handle_vibeos_bind_device();  /* bind NET, dev_type=1 */

    uint32_t hB = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 128);
    microkit_mr_set(1, hB); microkit_mr_set(2, 1); microkit_mr_set(3, 400);
    handle_vibeos_bind_device();  /* bind NET again — forced reuse */

    /* cap_policy_find must still return the FIRST registration (300) */
    uint32_t pd_out = 0, ch_out = 0;
    int found = cap_policy_find_ring0_service(VIBEOS_FUNC_CLASS_NET, &pd_out, &ch_out);
    ASSERT_EQ(found,   1u,   "registry still has exactly one NET entry");
    ASSERT_EQ(pd_out, 300u,  "registry entry is the original pd_handle 300");

    /* Second context's dev_handles should reference the existing PD */
    vibeos_ctx_t *ctxB = vibeos_find(hB);
    ASSERT_EQ(ctxB->dev_handles[1], 300u,
              "OS-B dev_handles[NET] = original 300, not new 400");
    ASSERT_EQ(ctxB->device_flags & VIBEOS_DEV_NET, VIBEOS_DEV_NET,
              "OS-B device_flags: NET bit set");
}

static void test_non_reinvention_multiple_classes_independent(void)
{
    TEST("non_reinvention: different classes are independent");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 512);

    /* Bind all 5 device classes with distinct handles */
    for (uint32_t dt = 0; dt < VIBEOS_DEV_COUNT; dt++) {
        microkit_mr_set(1, h);
        microkit_mr_set(2, dt);
        microkit_mr_set(3, 100 + dt);  /* handles: 100,101,102,103,104 */
        handle_vibeos_bind_device();
        ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "bind each class: ok");
        ASSERT_EQ(_stub_mrs[2], 0u,        "bind each class: new (no preexisting)");
    }

    /* All 5 function classes are now registered */
    for (uint32_t fc = 1; fc <= CAP_POLICY_FUNC_CLASS_MAX; fc++) {
        uint32_t pd = 0;
        int found = cap_policy_find_ring0_service(fc, &pd, (uint32_t*)0);
        ASSERT_EQ(found, 1u, "each class registered");
        ASSERT_EQ(pd, 100 + fc - 1, "correct pd_handle per class");
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test Group 2: Non-escalation (ring-1 enforcement)
 *
 * Invariant: a caller must not pass a ring-0 system channel ID as the
 * dev_handle for MSG_VIBEOS_BIND_DEVICE.  Doing so is a privilege escalation
 * attempt and must be denied with VIBEOS_ERR_BIND_FAIL.
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_non_escalation_ring0_channel_rejected(void)
{
    TEST("non_escalation: ring-0 channel ID as dev_handle → BIND_FAIL");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);

    /* Attempt to bind SERIAL with CH_VIBEENGINE (40) as dev_handle.
     * This is the escalation vector: attacker passes a ring-0 channel ID
     * hoping the engine will grant access to that channel. */
    microkit_mr_set(1, h);
    microkit_mr_set(2, 0);   /* dev_type 0 = SERIAL */
    microkit_mr_set(3, 40u); /* CH_VIBEENGINE — ring-0 system channel */
    handle_vibeos_bind_device();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BIND_FAIL,
              "ring-0 channel as dev_handle: BIND_FAIL");
    ASSERT_EQ(_stub_mrs[1], 0u, "effective_handle = 0 on EPERM");
    ASSERT_EQ(_stub_mrs[2], 0u, "preexisting = 0 on EPERM");

    /* Verify nothing was registered in cap_policy */
    uint32_t pd = 0;
    int found = cap_policy_find_ring0_service(VIBEOS_FUNC_CLASS_SERIAL, &pd, (uint32_t*)0);
    ASSERT_EQ(found, 0u, "cap_policy registry unchanged after rejected bind");

    /* Verify context device_flags unchanged */
    vibeos_ctx_t *ctx = vibeos_find(h);
    ASSERT_EQ(ctx->device_flags, 0u, "device_flags unchanged after rejected bind");
}

static void test_non_escalation_nameserver_channel_rejected(void)
{
    TEST("non_escalation: CH_NAMESERVER (18) as dev_handle → BIND_FAIL");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_FREEBSD, VIBEOS_ARCH_AARCH64, 128);
    microkit_mr_set(1, h);
    microkit_mr_set(2, 1);   /* dev_type 1 = NET */
    microkit_mr_set(3, 18u); /* CH_NAMESERVER — ring-0 */
    handle_vibeos_bind_device();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BIND_FAIL, "CH_NAMESERVER as dev_handle: BIND_FAIL");
}

static void test_non_escalation_normal_handle_allowed(void)
{
    TEST("non_escalation: legitimate PD handle (non-ring-0) is allowed");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);

    /* dev_handle=100 is not in the ring-0 channel list — must succeed */
    microkit_mr_set(1, h);
    microkit_mr_set(2, 0);    /* dev_type 0 = SERIAL */
    microkit_mr_set(3, 100u); /* normal PD handle */
    handle_vibeos_bind_device();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK,  "normal handle: ok");
    ASSERT_EQ(_stub_mrs[1], 100u,       "effective_handle = 100");
    ASSERT_EQ(_stub_mrs[2], 0u,         "preexisting = 0 (new registration)");

    vibeos_ctx_t *ctx = vibeos_find(h);
    ASSERT_EQ(ctx->dev_handles[0],           100u,           "dev_handles[SERIAL] = 100");
    ASSERT_EQ(ctx->device_flags & VIBEOS_DEV_SERIAL, VIBEOS_DEV_SERIAL,
              "device_flags SERIAL bit set");
}

static void test_non_escalation_all_known_ring0_channels(void)
{
    TEST("non_escalation: all known ring-0 channel IDs are rejected");

    for (uint32_t i = 0; i < RING0_CH_N; i++) {
        reset_state();
        uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 128);

        microkit_mr_set(1, h);
        microkit_mr_set(2, 0);                   /* dev_type SERIAL */
        microkit_mr_set(3, g_ring0_channels[i]); /* ring-0 channel ID */
        handle_vibeos_bind_device();

        ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BIND_FAIL,
                  "ring-0 channel rejected");
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test Group 3: WASM load + boot + snapshot + restore
 *
 * Invariant: context state and device bindings survive a snapshot/restore
 * round-trip.  "Module state survives" means the OS state (BOOTING), device
 * flags, and device handles are identical before and after the round-trip.
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_wasm_load_boot_snapshot_restore(void)
{
    TEST("wasm_load_boot_snapshot_restore: state survives round-trip");
    reset_state();

    /* 1. Create context */
    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 512);
    ASSERT_NE(h, 0u, "create: handle non-zero");

    /* 2. Bind a device */
    microkit_mr_set(1, h); microkit_mr_set(2, 0); microkit_mr_set(3, 200u);
    handle_vibeos_bind_device();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "bind SERIAL: ok");

    /* 3. Load WASM module */
    write_valid_wasm_magic();
    microkit_mr_set(1, h);
    microkit_mr_set(2, VIBEOS_MODULE_TYPE_WASM);
    microkit_mr_set(3, 64u);
    handle_vibeos_load_module();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "load WASM: ok");
    uint32_t swap_id = (uint32_t)_stub_mrs[1];
    ASSERT_NE(swap_id, 0u, "swap_id non-zero");

    /* 4. Boot context */
    microkit_mr_set(1, h);
    handle_vibeos_boot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "boot: ok");
    ASSERT_EQ(vibeos_find(h)->state, VIBEOS_STATE_BOOTING, "state: BOOTING");

    /* Record state before snapshot */
    vibeos_ctx_t before = *vibeos_find(h);

    /* 5. Snapshot */
    microkit_mr_set(1, h);
    handle_vibeos_snapshot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "snapshot: ok");
    uint32_t snap_lo = (uint32_t)_stub_mrs[1];

    /* 6. Create a second context and restore snapshot into it */
    uint32_t h2 = do_create(VIBEOS_TYPE_FREEBSD, VIBEOS_ARCH_AARCH64, 128);
    ASSERT_NE(h2, 0u, "create destination context");

    microkit_mr_set(1, h2);
    microkit_mr_set(2, snap_lo);
    microkit_mr_set(3, 0u);         /* snap_hi */
    handle_vibeos_restore();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "restore: ok");

    /* 7. Verify module state survived round-trip */
    vibeos_ctx_t *after = vibeos_find(h2);
    ASSERT_EQ(after->state,        before.state,        "state preserved");
    ASSERT_EQ(after->os_type,      before.os_type,      "os_type preserved");
    ASSERT_EQ(after->ram_mb,       before.ram_mb,       "ram_mb preserved");
    ASSERT_EQ(after->device_flags, before.device_flags, "device_flags preserved");
    ASSERT_EQ(after->dev_handles[0], before.dev_handles[0],
              "dev_handles[SERIAL] preserved");
}

static void test_snapshot_bad_handle(void)
{
    TEST("snapshot_bad_handle: VIBEOS_ERR_BAD_HANDLE");
    reset_state();

    microkit_mr_set(1, 0xDEADu);
    handle_vibeos_snapshot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_HANDLE, "snapshot bad handle: error");
}

static void test_restore_invalid_snap(void)
{
    TEST("restore_invalid_snap: VIBEOS_ERR_BAD_HANDLE for missing snapshot");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 128);
    microkit_mr_set(1, h);
    microkit_mr_set(2, 99u); /* no snapshot at slot 99 */
    microkit_mr_set(3, 0u);
    handle_vibeos_restore();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_HANDLE, "restore bad snap_lo: error");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test Group 4: Migrate
 *
 * Migration simulation: snapshot source context, destroy it, restore to a
 * fresh context, verify device handles reconnect (device_flags and
 * dev_handles match the original context's bindings).
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_migrate_device_handles_reconnect(void)
{
    TEST("migrate: snapshot + destroy + restore → device handles reconnect");
    reset_state();

    /* 1. Create and configure source context */
    uint32_t hSrc = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);
    ASSERT_NE(hSrc, 0u, "create source context");

    /* Bind NET (dev_type=1) and BLOCK (dev_type=2) */
    microkit_mr_set(1, hSrc); microkit_mr_set(2, 1u); microkit_mr_set(3, 500u);
    handle_vibeos_bind_device();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "source: bind NET ok");

    microkit_mr_set(1, hSrc); microkit_mr_set(2, 2u); microkit_mr_set(3, 600u);
    handle_vibeos_bind_device();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "source: bind BLOCK ok");

    /* Boot source */
    microkit_mr_set(1, hSrc);
    handle_vibeos_boot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "source: boot ok");

    uint32_t src_device_flags    = vibeos_find(hSrc)->device_flags;
    uint32_t src_net_handle      = vibeos_find(hSrc)->dev_handles[1];
    uint32_t src_block_handle    = vibeos_find(hSrc)->dev_handles[2];
    uint32_t src_state           = vibeos_find(hSrc)->state;

    /* 2. Snapshot source */
    microkit_mr_set(1, hSrc);
    handle_vibeos_snapshot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "snapshot source: ok");
    uint32_t snap_lo = (uint32_t)_stub_mrs[1];

    /* 3. Destroy source context */
    microkit_mr_set(1, hSrc);
    handle_vibeos_destroy();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "destroy source: ok");

    /* Source slot is now free */
    ASSERT_EQ((uintptr_t)vibeos_find(hSrc), 0u, "source context gone after destroy");

    /* 4. Create destination context */
    uint32_t hDst = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 128);
    ASSERT_NE(hDst, 0u, "create destination context");

    /* 5. Restore snapshot into destination */
    microkit_mr_set(1, hDst);
    microkit_mr_set(2, snap_lo);
    microkit_mr_set(3, 0u);
    handle_vibeos_restore();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "restore to destination: ok");

    /* 6. Verify device handles reconnected */
    vibeos_ctx_t *dst = vibeos_find(hDst);
    ASSERT_EQ(dst->device_flags, src_device_flags, "device_flags reconnected");
    ASSERT_EQ(dst->dev_handles[1], src_net_handle,   "NET handle reconnected");
    ASSERT_EQ(dst->dev_handles[2], src_block_handle, "BLOCK handle reconnected");
    ASSERT_EQ(dst->state,          src_state,         "context state preserved");
}

static void test_migrate_original_context_unreachable_after_destroy(void)
{
    TEST("migrate: source context unreachable after destroy");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 128);
    microkit_mr_set(1, h);
    handle_vibeos_destroy();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "destroy: ok");

    /* Any subsequent operation on the destroyed handle must fail */
    microkit_mr_set(1, h);
    handle_vibeos_boot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_HANDLE, "boot destroyed handle: BAD_HANDLE");

    microkit_mr_set(1, h);
    handle_vibeos_snapshot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_HANDLE, "snapshot destroyed handle: BAD_HANDLE");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test Group 5: Double-boot (EALREADY)
 *
 * Invariant: MSG_VIBEOS_BOOT on a context that is already booting/running
 * must return VIBEOS_ERR_BAD_STATE.  Callers must not rely on idempotent
 * boot semantics; the state machine is strictly CREATING → BOOTING.
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_double_boot_returns_bad_state(void)
{
    TEST("double_boot: second BOOT on BOOTING context → VIBEOS_ERR_BAD_STATE");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 512);
    ASSERT_EQ(vibeos_find(h)->state, VIBEOS_STATE_CREATING, "initial: CREATING");

    /* First boot: CREATING → BOOTING */
    microkit_mr_set(1, h);
    handle_vibeos_boot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "first boot: ok");
    ASSERT_EQ(vibeos_find(h)->state, VIBEOS_STATE_BOOTING, "state: BOOTING");

    /* Second boot: must be rejected (EALREADY / bad-state) */
    microkit_mr_set(1, h);
    handle_vibeos_boot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_STATE,
              "second boot (EALREADY): VIBEOS_ERR_BAD_STATE");

    /* State must remain BOOTING — not regressed to CREATING */
    ASSERT_EQ(vibeos_find(h)->state, VIBEOS_STATE_BOOTING,
              "state still BOOTING after rejected double-boot");
}

static void test_double_boot_bad_handle(void)
{
    TEST("double_boot: boot with invalid handle → VIBEOS_ERR_BAD_HANDLE");
    reset_state();

    microkit_mr_set(1, 0xCAFEu);
    handle_vibeos_boot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_HANDLE, "boot invalid handle: BAD_HANDLE");
}

static void test_boot_requires_creating_state_after_restore(void)
{
    TEST("double_boot: boot is invalid on restored (BOOTING) context");
    reset_state();

    /* Create, boot, snapshot, restore to a fresh context */
    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 128);
    microkit_mr_set(1, h); handle_vibeos_boot();  /* → BOOTING */

    microkit_mr_set(1, h); handle_vibeos_snapshot();
    uint32_t snap = (uint32_t)_stub_mrs[1];

    uint32_t h2 = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 64);
    microkit_mr_set(1, h2); microkit_mr_set(2, snap); microkit_mr_set(3, 0u);
    handle_vibeos_restore();

    /* Restored context is in BOOTING state — boot must fail */
    microkit_mr_set(1, h2);
    handle_vibeos_boot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_STATE,
              "boot on restored BOOTING context: BAD_STATE");
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  agentOS VibeOS Contract Test Suite                          ║\n");
    printf("║  Tests: non-reinvention, non-escalation, snapshot/restore,   ║\n");
    printf("║         migrate, double-boot                                  ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    printf("\n── Group 1: Non-reinvention ──────────────────────────────────\n");
    test_non_reinvention_returns_existing_handle();
    test_non_reinvention_no_second_pd_created();
    test_non_reinvention_multiple_classes_independent();

    printf("\n── Group 2: Non-escalation (ring-1 enforcement) ─────────────\n");
    test_non_escalation_ring0_channel_rejected();
    test_non_escalation_nameserver_channel_rejected();
    test_non_escalation_normal_handle_allowed();
    test_non_escalation_all_known_ring0_channels();

    printf("\n── Group 3: WASM load + boot + snapshot + restore ────────────\n");
    test_wasm_load_boot_snapshot_restore();
    test_snapshot_bad_handle();
    test_restore_invalid_snap();

    printf("\n── Group 4: Migrate ──────────────────────────────────────────\n");
    test_migrate_device_handles_reconnect();
    test_migrate_original_context_unreachable_after_destroy();

    printf("\n── Group 5: Double-boot (EALREADY) ───────────────────────────\n");
    test_double_boot_returns_bad_state();
    test_double_boot_bad_handle();
    test_boot_requires_creating_state_after_restore();

    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    if (tests_failed > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
