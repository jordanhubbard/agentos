/*
 * agentOS VibeOS Lifecycle — Unit Tests
 *
 * Tests the VibeOS lifecycle handlers via host stubs:
 *   MSG_VIBEOS_CREATE               — allocate OS context
 *   MSG_VIBEOS_BIND_DEVICE          — attach ring-0 device PD (non-reinvention enforced)
 *   MSG_VIBEOS_BOOT                 — transition CREATING → BOOTING
 *   MSG_VIBEOS_LOAD_MODULE          — install WASM/ELF via vibe_swap
 *   MSG_VIBEOS_CHECK_SERVICE_EXISTS — query cap_policy for existing ring-0 service
 *
 * Build:  cc -o /tmp/test_vibeos_lifecycle \
 *             tests/test_vibeos_lifecycle.c \
 *             -I tests \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST
 * Run:    /tmp/test_vibeos_lifecycle
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* ── Host-side stubs ────────────────────────────────────────────────── */

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

/* Stub staging region (the actual vibe_engine uses setvar_vaddr) */
static uint8_t _stub_staging[0x400000];
static uintptr_t vibe_staging_vaddr;

/* Barrier stubs */
static inline void agentos_wmb(void) {}

/* ── Constants ──────────────────────────────────────────────────────── */

#define VIBEOS_TYPE_LINUX     0x01u
#define VIBEOS_TYPE_FREEBSD   0x02u
#define VIBEOS_ARCH_AARCH64   0x01u
#define VIBEOS_ARCH_X86_64    0x02u
#define VIBEOS_DEV_SERIAL     (1u << 0)
#define VIBEOS_DEV_NET        (1u << 1)
#define VIBEOS_DEV_BLOCK      (1u << 2)
#define VIBEOS_DEV_USB        (1u << 3)
#define VIBEOS_DEV_FB         (1u << 4)
#define VIBEOS_FUNC_CLASS_SERIAL  0x01u
#define VIBEOS_FUNC_CLASS_NET     0x02u
#define VIBEOS_FUNC_CLASS_BLOCK   0x03u
#define VIBEOS_FUNC_CLASS_USB     0x04u
#define VIBEOS_FUNC_CLASS_FB      0x05u
#define VIBEOS_MODULE_TYPE_WASM   1u
#define VIBEOS_MODULE_TYPE_ELF    2u
#define VIBEOS_STATE_CREATING     0
#define VIBEOS_STATE_BOOTING      1
#define VIBEOS_STATE_RUNNING      2
#define VIBEOS_STATE_DEAD         4
#define VIBEOS_OK                 0
#define VIBEOS_ERR_NO_SLOTS       1
#define VIBEOS_ERR_BAD_HANDLE     2
#define VIBEOS_ERR_BAD_OS_TYPE    3
#define VIBEOS_ERR_DEVICE_UNAVAILABLE 4
#define VIBEOS_ERR_WASM_LOAD_FAIL 6
#define VIBEOS_ERR_BAD_MODULE_TYPE 9
#define VIBEOS_ERR_BAD_STATE      10
#define VIBEOS_ERR_BAD_FUNC_CLASS 11
#define MSG_VIBEOS_CREATE               0x2401
#define MSG_VIBEOS_BIND_DEVICE          0x2405
#define MSG_VIBEOS_BOOT                 0x240A
#define MSG_VIBEOS_LOAD_MODULE          0x240B
#define MSG_VIBEOS_CHECK_SERVICE_EXISTS 0x240C
#define CAP_POLICY_FUNC_CLASS_MAX       0x05u

/* Additional states and errors for the handlers added below */
#define VIBEOS_STATE_PAUSED     3
#define VIBEOS_ERR_NO_HANDLE    14
#define VIBEOS_ERR_WRONG_STATE  15
#define VIBEOS_ERR_NOT_IMPL     16
#define VIBEOS_ERR_BAD_TYPE     12
#define STAGING_SIZE                    0x400000UL

struct vibeos_create_req {
    uint8_t  os_type;
    uint8_t  arch;
    uint8_t  _pad[2];
    uint32_t ram_mb;
    uint32_t cpu_budget_us;
    uint32_t cpu_period_us;
    uint32_t device_flags;
    uint8_t  wasm_hash[32];
};

/* ── Inline copies of the VibeOS lifecycle state and handlers ───────── */

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

/* Ring-0 non-reinvention registry (mirrors cap_policy.c) */
typedef struct {
    uint32_t pd_handle;
    uint32_t channel_id;
    bool     registered;
} ring0_svc_entry_t;
static ring0_svc_entry_t g_ring0_registry[CAP_POLICY_FUNC_CLASS_MAX + 1];

static int cap_policy_find_ring0_service(uint32_t fc, uint32_t *pd, uint32_t *ch) {
    if (fc < 1 || fc > CAP_POLICY_FUNC_CLASS_MAX) return 0;
    if (!g_ring0_registry[fc].registered) return 0;
    if (pd) *pd = g_ring0_registry[fc].pd_handle;
    if (ch) *ch = g_ring0_registry[fc].channel_id;
    return 1;
}
static int cap_policy_register_ring0_service(uint32_t fc, uint32_t pd, uint32_t ch) {
    if (fc < 1 || fc > CAP_POLICY_FUNC_CLASS_MAX) return -1;
    if (g_ring0_registry[fc].registered) return -1;
    g_ring0_registry[fc].pd_handle  = pd;
    g_ring0_registry[fc].channel_id = ch;
    g_ring0_registry[fc].registered = true;
    return 0;
}
static void cap_policy_unregister_ring0_service(uint32_t fc) {
    if (fc < 1 || fc > CAP_POLICY_FUNC_CLASS_MAX) return;
    g_ring0_registry[fc] = (ring0_svc_entry_t){0};
}

static uint32_t next_proposal_id = 1;

static vibeos_ctx_t *vibeos_find(uint32_t h) {
    for (int i = 0; i < MAX_VIBEOS_INSTANCES; i++)
        if (g_vibeos[i].handle == h) return &g_vibeos[i];
    return (void*)0;
}
static vibeos_ctx_t *vibeos_alloc(void) {
    for (int i = 0; i < MAX_VIBEOS_INSTANCES; i++)
        if (g_vibeos[i].handle == 0) return &g_vibeos[i];
    return (void*)0;
}
static uint32_t dev_type_to_func_class(uint32_t dt) { return dt + 1; }

static bool validate_wasm_header(const uint8_t *d, uint32_t sz) {
    if (sz < 8) return false;
    return d[0]==0x00 && d[1]==0x61 && d[2]==0x73 && d[3]==0x6D;
}

static microkit_msginfo handle_vibeos_create(void) {
    const struct vibeos_create_req *req = (const struct vibeos_create_req *)vibe_staging_vaddr;
    vibeos_ctx_t *ctx = vibeos_alloc();
    if (!ctx) { microkit_mr_set(0, VIBEOS_ERR_NO_SLOTS); microkit_mr_set(1, 0); return microkit_msginfo_new(0,2); }
    if (req->os_type != VIBEOS_TYPE_LINUX && req->os_type != VIBEOS_TYPE_FREEBSD) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_OS_TYPE); microkit_mr_set(1, 0); return microkit_msginfo_new(0,2);
    }
    uint32_t h = g_next_vibeos_handle++;
    if (g_next_vibeos_handle == 0) g_next_vibeos_handle = 1;
    ctx->handle = h; ctx->os_type = req->os_type; ctx->arch = req->arch;
    ctx->state = VIBEOS_STATE_CREATING; ctx->ram_mb = req->ram_mb;
    ctx->device_flags = 0; ctx->uptime_ticks = 0;
    for (int i = 0; i < VIBEOS_DEV_COUNT; i++) { ctx->dev_handles[i] = 0; ctx->dev_channels[i] = 0; }
    microkit_mr_set(0, VIBEOS_OK); microkit_mr_set(1, h); return microkit_msginfo_new(0,2);
}

static microkit_msginfo handle_vibeos_bind_device(void) {
    uint32_t handle     = (uint32_t)microkit_mr_get(1);
    uint32_t dev_type   = (uint32_t)microkit_mr_get(2);
    uint32_t dev_handle = (uint32_t)microkit_mr_get(3);
    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) { microkit_mr_set(0, VIBEOS_ERR_BAD_HANDLE); microkit_mr_set(1,0); microkit_mr_set(2,0); return microkit_msginfo_new(0,3); }
    if (dev_type >= VIBEOS_DEV_COUNT) { microkit_mr_set(0, VIBEOS_ERR_DEVICE_UNAVAILABLE); microkit_mr_set(1,0); microkit_mr_set(2,0); return microkit_msginfo_new(0,3); }
    uint32_t fc = dev_type_to_func_class(dev_type);
    uint32_t ex_pd = 0, ex_ch = 0, preexisting = 0, effective_h = dev_handle;
    if (cap_policy_find_ring0_service(fc, &ex_pd, &ex_ch)) {
        effective_h = ex_pd; preexisting = 1;
        ctx->dev_handles[dev_type] = ex_pd; ctx->dev_channels[dev_type] = ex_ch;
        ctx->device_flags |= (1u << dev_type);
    } else {
        cap_policy_register_ring0_service(fc, dev_handle, 0);
        ctx->dev_handles[dev_type] = dev_handle; ctx->dev_channels[dev_type] = 0;
        ctx->device_flags |= (1u << dev_type);
    }
    microkit_mr_set(0, VIBEOS_OK); microkit_mr_set(1, effective_h); microkit_mr_set(2, preexisting);
    return microkit_msginfo_new(0,3);
}

static microkit_msginfo handle_vibeos_boot(void) {
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) { microkit_mr_set(0, VIBEOS_ERR_BAD_HANDLE); return microkit_msginfo_new(0,1); }
    if (ctx->state != VIBEOS_STATE_CREATING) { microkit_mr_set(0, VIBEOS_ERR_BAD_STATE); return microkit_msginfo_new(0,1); }
    ctx->state = VIBEOS_STATE_BOOTING;
    microkit_mr_set(0, VIBEOS_OK); return microkit_msginfo_new(0,1);
}

static microkit_msginfo handle_vibeos_load_module(void) {
    uint32_t handle      = (uint32_t)microkit_mr_get(1);
    uint32_t module_type = (uint32_t)microkit_mr_get(2);
    uint32_t module_size = (uint32_t)microkit_mr_get(3);
    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) { microkit_mr_set(0, VIBEOS_ERR_BAD_HANDLE); microkit_mr_set(1,0); return microkit_msginfo_new(0,2); }
    if (module_type != VIBEOS_MODULE_TYPE_WASM && module_type != VIBEOS_MODULE_TYPE_ELF) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_MODULE_TYPE); microkit_mr_set(1,0); return microkit_msginfo_new(0,2);
    }
    if (module_type == VIBEOS_MODULE_TYPE_WASM) {
        if (!validate_wasm_header((const uint8_t *)vibe_staging_vaddr, module_size)) {
            microkit_mr_set(0, VIBEOS_ERR_WASM_LOAD_FAIL); microkit_mr_set(1,0); return microkit_msginfo_new(0,2);
        }
    }
    uint32_t swap_id = next_proposal_id++;
    microkit_mr_set(0, VIBEOS_OK); microkit_mr_set(1, swap_id); return microkit_msginfo_new(0,2);
}

static microkit_msginfo handle_vibeos_check_service_exists(void) {
    uint32_t func_class = (uint32_t)microkit_mr_get(1);
    if (func_class < 1 || func_class > CAP_POLICY_FUNC_CLASS_MAX) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_FUNC_CLASS);
        microkit_mr_set(1,0); microkit_mr_set(2,0); microkit_mr_set(3,0);
        return microkit_msginfo_new(0,4);
    }
    uint32_t pd = 0, ch = 0;
    uint32_t exists = (uint32_t)cap_policy_find_ring0_service(func_class, &pd, &ch);
    microkit_mr_set(0, VIBEOS_OK); microkit_mr_set(1, exists);
    microkit_mr_set(2, pd); microkit_mr_set(3, ch);
    return microkit_msginfo_new(0,4);
}

/* ── Inline handler stubs for DESTROY, STATUS, LIST, UNBIND,
 *    SNAPSHOT, RESTORE, MIGRATE — mirror production logic using the
 *    test file's vibeos_ctx_t / g_vibeos[] / vibeos_find() types.
 *    microkit_ppcall() (VMM calls) are omitted; stubs return success.
 * ─────────────────────────────────────────────────────────────────── */

static microkit_msginfo handle_vos_destroy(void) {
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    ctx->handle = 0;
    ctx->state  = VIBEOS_STATE_DEAD;
    microkit_mr_set(0, VIBEOS_OK);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_vos_status(void) {
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, handle);
    microkit_mr_set(2, ctx->state);
    microkit_mr_set(3, ctx->os_type);
    microkit_mr_set(4, ctx->ram_mb);
    microkit_mr_set(5, ctx->device_flags);
    return microkit_msginfo_new(0, 6);
}

static microkit_msginfo handle_vos_list(void) {
    uint32_t offset = (uint32_t)microkit_mr_get(1);
    uint32_t count = 0, seen = 0;
    for (int i = 0; i < MAX_VIBEOS_INSTANCES && count < 16; i++) {
        if (g_vibeos[i].handle == 0) continue;
        if (seen < offset) { seen++; continue; }
        microkit_mr_set(2 + count, g_vibeos[i].handle);
        count++;
        seen++;
    }
    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, count);
    return microkit_msginfo_new(0, 2 + count);
}

static microkit_msginfo handle_vos_unbind_device(void) {
    uint32_t handle   = (uint32_t)microkit_mr_get(1);
    uint32_t dev_type = (uint32_t)microkit_mr_get(2);
    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (dev_type >= VIBEOS_DEV_COUNT) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_TYPE);
        return microkit_msginfo_new(0, 1);
    }
    ctx->device_flags &= ~(1u << dev_type);
    microkit_mr_set(0, VIBEOS_OK);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_vos_snapshot(void) {
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (ctx->state != VIBEOS_STATE_RUNNING && ctx->state != VIBEOS_STATE_PAUSED) {
        microkit_mr_set(0, VIBEOS_ERR_WRONG_STATE);
        return microkit_msginfo_new(0, 1);
    }
    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, handle);
    microkit_mr_set(2, 0);
    microkit_mr_set(3, 0);
    return microkit_msginfo_new(0, 4);
}

static microkit_msginfo handle_vos_restore(void) {
    uint32_t handle  = (uint32_t)microkit_mr_get(1);
    uint32_t snap_lo = (uint32_t)microkit_mr_get(2);
    uint32_t snap_hi = (uint32_t)microkit_mr_get(3);
    (void)snap_lo; (void)snap_hi;
    vibeos_ctx_t *ctx = vibeos_find(handle);
    if (!ctx) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    ctx->state = VIBEOS_STATE_BOOTING;
    microkit_mr_set(0, VIBEOS_OK);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_vos_migrate(void) {
    microkit_mr_set(0, VIBEOS_ERR_NOT_IMPL);
    return microkit_msginfo_new(0, 1);
}

/* ── Helpers for tests ──────────────────────────────────────────────── */

static void reset_state(void) {
    memset(g_vibeos, 0, sizeof(g_vibeos));
    memset(g_ring0_registry, 0, sizeof(g_ring0_registry));
    memset(_stub_staging, 0, sizeof(_stub_staging));
    g_next_vibeos_handle = 1;
    next_proposal_id = 1;
    vibe_staging_vaddr = (uintptr_t)_stub_staging;
}

static uint32_t do_create(uint8_t os_type, uint8_t arch, uint32_t ram_mb) {
    struct vibeos_create_req *req = (struct vibeos_create_req *)vibe_staging_vaddr;
    req->os_type = os_type;
    req->arch    = arch;
    req->ram_mb  = ram_mb;
    handle_vibeos_create();
    return (uint32_t)_stub_mrs[1];
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test framework
 * ══════════════════════════════════════════════════════════════════════════ */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)      printf("\n=== TEST: %s ===\n", (name))
#define ASSERT_EQ(a, b, msg)  do { \
    if ((int64_t)(a) != (int64_t)(b)) { \
        printf("  FAIL: %s (got %lld expected %lld)\n", (msg), \
               (long long)(a), (long long)(b)); tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)
#define ASSERT_NE(a, b, msg)  do { \
    if ((int64_t)(a) == (int64_t)(b)) { \
        printf("  FAIL: %s (got %lld, expected ≠ %lld)\n", (msg), \
               (long long)(a), (long long)(b)); tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_CREATE
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_create_linux(void) {
    TEST("vibeos_create_linux");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 512);
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "CREATE linux: ok");
    ASSERT_NE(h, 0, "handle non-zero");

    vibeos_ctx_t *ctx = vibeos_find(h);
    ASSERT_EQ(ctx != (void*)0, 1, "context found by handle");
    ASSERT_EQ(ctx->os_type, VIBEOS_TYPE_LINUX, "os_type linux");
    ASSERT_EQ(ctx->ram_mb, 512u, "ram_mb stored");
    ASSERT_EQ(ctx->state, VIBEOS_STATE_CREATING, "initial state: CREATING");
}

static void test_create_freebsd(void) {
    TEST("vibeos_create_freebsd");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_FREEBSD, VIBEOS_ARCH_AARCH64, 256);
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "CREATE freebsd: ok");
    ASSERT_NE(h, 0, "handle non-zero");
}

static void test_create_bad_os_type(void) {
    TEST("vibeos_create_bad_os_type");
    reset_state();

    uint32_t h = do_create(0xFF, VIBEOS_ARCH_X86_64, 512);
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_OS_TYPE, "CREATE bad os_type: error");
    ASSERT_EQ(h, 0u, "handle is 0 on error");
}

static void test_create_exhausts_slots(void) {
    TEST("vibeos_create_exhausts_slots");
    reset_state();

    for (int i = 0; i < MAX_VIBEOS_INSTANCES; i++)
        do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 128);

    do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 128);
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_NO_SLOTS, "CREATE beyond max: ERR_NO_SLOTS");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_BIND_DEVICE + non-reinvention
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_bind_device_new(void) {
    TEST("vibeos_bind_device_new_registration");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);
    microkit_mr_set(1, h);
    microkit_mr_set(2, 0);  /* dev_type 0 = SERIAL */
    microkit_mr_set(3, 100);/* dev_handle */
    handle_vibeos_bind_device();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "BIND_DEVICE new: ok");
    ASSERT_EQ(_stub_mrs[1], 100u, "effective_handle = requested");
    ASSERT_EQ(_stub_mrs[2], 0u,   "preexisting = 0 (new registration)");

    /* Verify context updated */
    vibeos_ctx_t *ctx = vibeos_find(h);
    ASSERT_EQ(ctx->dev_handles[0], 100u, "dev_handles[serial] stored");
    ASSERT_EQ(ctx->device_flags & VIBEOS_DEV_SERIAL, VIBEOS_DEV_SERIAL, "device_flags serial bit set");
}

static void test_bind_device_non_reinvention(void) {
    TEST("vibeos_bind_device_non_reinvention_forced_reuse");
    reset_state();

    /* First OS instance: registers serial PD with handle 100 */
    uint32_t h1 = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);
    microkit_mr_set(1, h1); microkit_mr_set(2, 0); microkit_mr_set(3, 100);
    handle_vibeos_bind_device();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "first bind: ok");
    ASSERT_EQ(_stub_mrs[2], 0u, "first bind: new (preexisting=0)");

    /* Second OS instance: tries to bind a NEW serial PD with handle 200 */
    uint32_t h2 = do_create(VIBEOS_TYPE_FREEBSD, VIBEOS_ARCH_X86_64, 128);
    microkit_mr_set(1, h2); microkit_mr_set(2, 0); microkit_mr_set(3, 200);
    handle_vibeos_bind_device();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK,  "second bind: ok (not error)");
    ASSERT_EQ(_stub_mrs[1], 100u, "effective_handle = EXISTING (not 200)");
    ASSERT_EQ(_stub_mrs[2], 1u,   "preexisting = 1 (reuse forced)");

    /* Second context's dev_handles must reference the existing PD */
    vibeos_ctx_t *ctx2 = vibeos_find(h2);
    ASSERT_EQ(ctx2->dev_handles[0], 100u, "second OS serial handle = existing 100");
}

static void test_bind_device_bad_handle(void) {
    TEST("vibeos_bind_device_bad_handle");
    reset_state();

    microkit_mr_set(1, 0xDEAD); microkit_mr_set(2, 0); microkit_mr_set(3, 1);
    handle_vibeos_bind_device();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_HANDLE, "BIND_DEVICE bad handle: error");
}

static void test_bind_device_bad_dev_type(void) {
    TEST("vibeos_bind_device_bad_dev_type");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 128);
    microkit_mr_set(1, h); microkit_mr_set(2, VIBEOS_DEV_COUNT); microkit_mr_set(3, 1);
    handle_vibeos_bind_device();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_DEVICE_UNAVAILABLE, "BIND_DEVICE bad dev_type: error");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_BOOT
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_boot_valid(void) {
    TEST("vibeos_boot_valid");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 512);
    ASSERT_EQ(vibeos_find(h)->state, VIBEOS_STATE_CREATING, "initial state CREATING");

    microkit_mr_set(1, h);
    handle_vibeos_boot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "BOOT: ok");
    ASSERT_EQ(vibeos_find(h)->state, VIBEOS_STATE_BOOTING, "state → BOOTING");
}

static void test_boot_wrong_state(void) {
    TEST("vibeos_boot_wrong_state");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 512);
    microkit_mr_set(1, h); handle_vibeos_boot();   /* transition to BOOTING */

    microkit_mr_set(1, h); handle_vibeos_boot();   /* BOOT again from BOOTING */
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_STATE, "BOOT from non-CREATING: ERR_BAD_STATE");
}

static void test_boot_bad_handle(void) {
    TEST("vibeos_boot_bad_handle");
    reset_state();

    microkit_mr_set(1, 0xDEAD);
    handle_vibeos_boot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_HANDLE, "BOOT bad handle: error");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_LOAD_MODULE
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_load_module_wasm(void) {
    TEST("vibeos_load_module_wasm");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);
    /* Write valid WASM magic to staging region */
    uint8_t *staging = (uint8_t *)vibe_staging_vaddr;
    staging[0] = 0x00; staging[1] = 0x61; staging[2] = 0x73; staging[3] = 0x6D;
    staging[4] = 0x01; staging[5] = 0x00; staging[6] = 0x00; staging[7] = 0x00;

    microkit_mr_set(1, h);
    microkit_mr_set(2, VIBEOS_MODULE_TYPE_WASM);
    microkit_mr_set(3, 64); /* module_size */
    handle_vibeos_load_module();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "LOAD_MODULE wasm: ok");
    ASSERT_NE(_stub_mrs[1], 0u, "swap_id non-zero");
}

static void test_load_module_bad_wasm_magic(void) {
    TEST("vibeos_load_module_bad_wasm_magic");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);
    /* Write invalid WASM magic */
    uint8_t *staging = (uint8_t *)vibe_staging_vaddr;
    staging[0] = 0xFF; staging[1] = 0xFF;

    microkit_mr_set(1, h);
    microkit_mr_set(2, VIBEOS_MODULE_TYPE_WASM);
    microkit_mr_set(3, 64);
    handle_vibeos_load_module();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_WASM_LOAD_FAIL, "LOAD_MODULE bad WASM magic: error");
}

static void test_load_module_elf(void) {
    TEST("vibeos_load_module_elf");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);
    microkit_mr_set(1, h);
    microkit_mr_set(2, VIBEOS_MODULE_TYPE_ELF);
    microkit_mr_set(3, 128);
    handle_vibeos_load_module();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "LOAD_MODULE elf: ok (no magic check)");
}

static void test_load_module_bad_type(void) {
    TEST("vibeos_load_module_bad_module_type");
    reset_state();

    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);
    microkit_mr_set(1, h);
    microkit_mr_set(2, 0xFF);
    microkit_mr_set(3, 64);
    handle_vibeos_load_module();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_MODULE_TYPE, "LOAD_MODULE bad type: error");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_CHECK_SERVICE_EXISTS
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_check_service_not_registered(void) {
    TEST("vibeos_check_service_not_registered");
    reset_state();

    microkit_mr_set(1, VIBEOS_FUNC_CLASS_SERIAL);
    handle_vibeos_check_service_exists();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "CHECK_SERVICE_EXISTS: ok");
    ASSERT_EQ(_stub_mrs[1], 0u, "exists = 0 (not registered)");
    ASSERT_EQ(_stub_mrs[2], 0u, "pd_handle = 0");
}

static void test_check_service_registered(void) {
    TEST("vibeos_check_service_registered");
    reset_state();

    /* Register a serial service */
    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_X86_64, 256);
    microkit_mr_set(1, h); microkit_mr_set(2, 0); microkit_mr_set(3, 77);
    handle_vibeos_bind_device();

    microkit_mr_set(1, VIBEOS_FUNC_CLASS_SERIAL);
    handle_vibeos_check_service_exists();

    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "CHECK_SERVICE_EXISTS: ok");
    ASSERT_EQ(_stub_mrs[1], 1u, "exists = 1 (registered)");
    ASSERT_EQ(_stub_mrs[2], 77u, "pd_handle = 77");
}

static void test_check_service_bad_func_class(void) {
    TEST("vibeos_check_service_bad_func_class");
    reset_state();

    microkit_mr_set(1, 0);
    handle_vibeos_check_service_exists();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_FUNC_CLASS, "CHECK: func_class 0: error");

    microkit_mr_set(1, CAP_POLICY_FUNC_CLASS_MAX + 1);
    handle_vibeos_check_service_exists();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_BAD_FUNC_CLASS, "CHECK: func_class > MAX: error");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_DESTROY
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_destroy_valid(void) {
    TEST("vibeos_destroy_valid");
    reset_state();
    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_AARCH64, 512);
    ASSERT_NE(h, 0, "create succeeded");
    microkit_mr_set(1, h);
    handle_vos_destroy();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "DESTROY: ok");
    ASSERT_EQ(vibeos_find(h) == (void*)0, 1, "handle no longer findable");
}

static void test_destroy_bad_handle(void) {
    TEST("vibeos_destroy_bad_handle");
    reset_state();
    microkit_mr_set(1, 0xDEADBEEFu);
    handle_vos_destroy();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_NO_HANDLE, "DESTROY: bad handle");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_STATUS
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_status_valid(void) {
    TEST("vibeos_status_valid");
    reset_state();
    uint32_t h = do_create(VIBEOS_TYPE_FREEBSD, VIBEOS_ARCH_AARCH64, 256);
    ASSERT_NE(h, 0, "create succeeded");
    microkit_mr_set(1, h);
    handle_vos_status();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "STATUS: ok");
    ASSERT_EQ(_stub_mrs[1], h, "STATUS: handle matches");
    ASSERT_EQ(_stub_mrs[2], VIBEOS_STATE_CREATING, "STATUS: state CREATING");
    ASSERT_EQ(_stub_mrs[3], VIBEOS_TYPE_FREEBSD, "STATUS: os_type FreeBSD");
    ASSERT_EQ(_stub_mrs[4], 256, "STATUS: ram_mb");
}

static void test_status_bad_handle(void) {
    TEST("vibeos_status_bad_handle");
    reset_state();
    microkit_mr_set(1, 0xDEADBEEFu);
    handle_vos_status();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_NO_HANDLE, "STATUS: bad handle");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_LIST
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_list_empty(void) {
    TEST("vibeos_list_empty");
    reset_state();
    microkit_mr_set(1, 0);
    handle_vos_list();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "LIST: ok");
    ASSERT_EQ(_stub_mrs[1], 0, "LIST: count=0 when empty");
}

static void test_list_two_instances(void) {
    TEST("vibeos_list_two_instances");
    reset_state();
    uint32_t h1 = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_AARCH64, 256);
    uint32_t h2 = do_create(VIBEOS_TYPE_FREEBSD, VIBEOS_ARCH_AARCH64, 512);
    ASSERT_NE(h1, 0, "create h1");
    ASSERT_NE(h2, 0, "create h2");
    microkit_mr_set(1, 0);
    handle_vos_list();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "LIST: ok");
    ASSERT_EQ(_stub_mrs[1], 2, "LIST: count=2");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_UNBIND_DEVICE
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_unbind_device_valid(void) {
    TEST("vibeos_unbind_device_valid");
    reset_state();
    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_AARCH64, 512);
    /* Bind serial (dev_type index 0) first */
    microkit_mr_set(1, h);
    microkit_mr_set(2, 0);
    microkit_mr_set(3, 99);
    handle_vibeos_bind_device();
    vibeos_ctx_t *ctx = vibeos_find(h);
    ASSERT_NE((int)(ctx->device_flags & VIBEOS_DEV_SERIAL), 0,
              "BIND: serial bit set");
    /* Now unbind */
    microkit_mr_set(1, h);
    microkit_mr_set(2, 0);
    handle_vos_unbind_device();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "UNBIND: ok");
    ASSERT_EQ(ctx->device_flags & VIBEOS_DEV_SERIAL, 0u,
              "UNBIND: serial bit cleared");
}

static void test_unbind_device_bad_handle(void) {
    TEST("vibeos_unbind_device_bad_handle");
    reset_state();
    microkit_mr_set(1, 0xDEADBEEFu);
    microkit_mr_set(2, 0);
    handle_vos_unbind_device();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_NO_HANDLE, "UNBIND: bad handle");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_SNAPSHOT
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_snapshot_wrong_state(void) {
    TEST("vibeos_snapshot_wrong_state");
    reset_state();
    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_AARCH64, 512);
    /* Default state is CREATING — snapshot should reject */
    microkit_mr_set(1, h);
    handle_vos_snapshot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_WRONG_STATE,
              "SNAPSHOT: rejects CREATING state");
}

static void test_snapshot_from_running(void) {
    TEST("vibeos_snapshot_from_running");
    reset_state();
    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_AARCH64, 512);
    vibeos_ctx_t *ctx = vibeos_find(h);
    ctx->state = VIBEOS_STATE_RUNNING;
    microkit_mr_set(1, h);
    handle_vos_snapshot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "SNAPSHOT: ok from RUNNING");
    ASSERT_EQ(_stub_mrs[1], h, "SNAPSHOT: handle in reply");
}

static void test_snapshot_from_paused(void) {
    TEST("vibeos_snapshot_from_paused");
    reset_state();
    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_AARCH64, 512);
    vibeos_ctx_t *ctx = vibeos_find(h);
    ctx->state = VIBEOS_STATE_PAUSED;
    microkit_mr_set(1, h);
    handle_vos_snapshot();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "SNAPSHOT: ok from PAUSED");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_RESTORE
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_restore_valid(void) {
    TEST("vibeos_restore_valid");
    reset_state();
    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_AARCH64, 512);
    microkit_mr_set(1, h);
    microkit_mr_set(2, 0xABCD1234u);
    microkit_mr_set(3, 0x00000001u);
    handle_vos_restore();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_OK, "RESTORE: ok");
    vibeos_ctx_t *ctx = vibeos_find(h);
    ASSERT_EQ(ctx->state, VIBEOS_STATE_BOOTING, "RESTORE: state → BOOTING");
}

static void test_restore_bad_handle(void) {
    TEST("vibeos_restore_bad_handle");
    reset_state();
    microkit_mr_set(1, 0xDEADBEEFu);
    microkit_mr_set(2, 0);
    microkit_mr_set(3, 0);
    handle_vos_restore();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_NO_HANDLE, "RESTORE: bad handle");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: MSG_VIBEOS_MIGRATE
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_migrate_not_impl(void) {
    TEST("vibeos_migrate_not_impl");
    reset_state();
    uint32_t h = do_create(VIBEOS_TYPE_LINUX, VIBEOS_ARCH_AARCH64, 512);
    microkit_mr_set(1, h);
    microkit_mr_set(2, 1);
    handle_vos_migrate();
    ASSERT_EQ(_stub_mrs[0], VIBEOS_ERR_NOT_IMPL,
              "MIGRATE: returns ERR_NOT_IMPL (Phase 4+ placeholder)");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  agentOS VibeOS Lifecycle — Test Suite                   ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    test_create_linux();
    test_create_freebsd();
    test_create_bad_os_type();
    test_create_exhausts_slots();

    test_bind_device_new();
    test_bind_device_non_reinvention();
    test_bind_device_bad_handle();
    test_bind_device_bad_dev_type();

    test_boot_valid();
    test_boot_wrong_state();
    test_boot_bad_handle();

    test_load_module_wasm();
    test_load_module_bad_wasm_magic();
    test_load_module_elf();
    test_load_module_bad_type();

    test_check_service_not_registered();
    test_check_service_registered();
    test_check_service_bad_func_class();

    test_destroy_valid();
    test_destroy_bad_handle();

    test_status_valid();
    test_status_bad_handle();

    test_list_empty();
    test_list_two_instances();

    test_unbind_device_valid();
    test_unbind_device_bad_handle();

    test_snapshot_wrong_state();
    test_snapshot_from_running();
    test_snapshot_from_paused();

    test_restore_valid();
    test_restore_bad_handle();

    test_migrate_not_impl();

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    if (tests_failed > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
