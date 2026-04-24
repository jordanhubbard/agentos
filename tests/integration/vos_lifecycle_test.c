/*
 * vos_lifecycle_test.c — VOS lifecycle integration test suite
 *
 * Exercises the FULL vibeOS lifecycle in multi-step sequences:
 *   CREATE → STATUS → SNAPSHOT → RESTORE → DESTROY
 *
 * Unlike the unit tests in tests/api/, every test here exercises at least two
 * consecutive operations to verify that state is correctly propagated across
 * the instance table and blob store.
 *
 * Build:
 *   cc -DAGENTOS_TEST_HOST \
 *      -I kernel/agentos-root-task/include \
 *      -I tests/harness \
 *      tests/integration/vos_lifecycle_test.c \
 *      kernel/agentos-root-task/src/vos_create.c \
 *      kernel/agentos-root-task/src/vos_destroy.c \
 *      kernel/agentos-root-task/src/vos_snapshot.c \
 *      kernel/agentos-root-task/src/vos_restore.c \
 *      -o test_lifecycle && ./test_lifecycle
 *
 * Expected output on pass: last line "TAP_DONE:0"
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef AGENTOS_TEST_HOST
#error "vos_lifecycle_test.c must be compiled with -DAGENTOS_TEST_HOST"
#endif

#include <stdio.h>    /* include system stdio before any project headers */
#include <stdint.h>
#include <string.h>

/* Test harness (TAP output via stdio) */
#include "sel4_test_harness.h"

/* VOS implementation types and API */
#include "vos_types.h"
#include "vos_snap_store.h"

/* ── Test helpers ────────────────────────────────────────────────────────────── */

/*
 * setup() — reset both the instance table and snapshot blob store.
 * Call at the top of each test function for a clean slate.
 */
static void setup(void)
{
    vos_create_init();
    vos_snapshot_init();
}

/*
 * make_spec() — return a vos_spec_t with sensible defaults.
 */
static vos_spec_t make_spec(vos_os_type_t os_type, const char *label)
{
    vos_spec_t s;
    memset(&s, 0, sizeof(s));
    s.os_type       = os_type;
    s.vcpu_count    = 1;
    s.cpu_quota_pct = 50;
    s.memory_pages  = VOS_SPEC_MIN_PAGES;
    s.cpu_affinity  = 0xFFFFFFFFu;
    if (label)
        strncpy(s.label, label, sizeof(s.label) - 1);
    return s;
}

/* ════════════════════════════════════════════════════════════════════════════════
 * Group A: CREATE → STATUS sequences
 * ════════════════════════════════════════════════════════════════════════════════ */

static void test_create_then_status_ok(void)
{
    SEL4_TEST_BEGIN("create_then_status_ok");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "linux-a");
    vos_handle_t h    = VOS_HANDLE_INVALID;

    vos_err_t rc = vos_create(&spec, &h);
    SEL4_ASSERT_OK(rc, "create returns VOS_ERR_OK");
    SEL4_ASSERT_NE(h, (int64_t)VOS_HANDLE_INVALID, "create sets valid handle");

    vos_status_t st;
    rc = vos_get_status(h, &st);
    SEL4_ASSERT_OK(rc, "status returns VOS_ERR_OK");
    SEL4_ASSERT_EQ(st.state, VOS_STATE_RUNNING, "state is RUNNING after create");
}

static void test_create_sets_correct_os_type(void)
{
    SEL4_TEST_BEGIN("create_sets_correct_os_type");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "linux-b");
    vos_handle_t h    = VOS_HANDLE_INVALID;

    vos_create(&spec, &h);

    vos_status_t st;
    vos_get_status(h, &st);
    SEL4_ASSERT_EQ(st.os_type, VOS_OS_LINUX, "os_type is VOS_OS_LINUX");
}

static void test_create_freebsd_os_type(void)
{
    SEL4_TEST_BEGIN("create_freebsd_os_type");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_FREEBSD, "freebsd-a");
    vos_handle_t h    = VOS_HANDLE_INVALID;

    vos_create(&spec, &h);

    vos_status_t st;
    vos_get_status(h, &st);
    SEL4_ASSERT_EQ(st.os_type, VOS_OS_FREEBSD, "os_type is VOS_OS_FREEBSD");
}

static void test_create_multiple_guests(void)
{
    SEL4_TEST_BEGIN("create_multiple_guests");
    setup();

    vos_handle_t h0 = VOS_HANDLE_INVALID;
    vos_handle_t h1 = VOS_HANDLE_INVALID;
    vos_handle_t h2 = VOS_HANDLE_INVALID;
    vos_spec_t   spec;

    spec = make_spec(VOS_OS_LINUX, "multi");
    SEL4_ASSERT_OK(vos_create(&spec, &h0), "guest 0 create ok");
    spec = make_spec(VOS_OS_LINUX, "multi");
    SEL4_ASSERT_OK(vos_create(&spec, &h1), "guest 1 create ok");
    spec = make_spec(VOS_OS_LINUX, "multi");
    SEL4_ASSERT_OK(vos_create(&spec, &h2), "guest 2 create ok");

    SEL4_ASSERT_NE(h0, (int64_t)VOS_HANDLE_INVALID, "handle 0 valid");
    SEL4_ASSERT_NE(h1, (int64_t)VOS_HANDLE_INVALID, "handle 1 valid");
    SEL4_ASSERT_NE(h2, (int64_t)VOS_HANDLE_INVALID, "handle 2 valid");
    SEL4_ASSERT_NE(h0, h1, "handles 0 and 1 distinct");
    SEL4_ASSERT_NE(h1, h2, "handles 1 and 2 distinct");
}

static void test_create_beyond_max(void)
{
    SEL4_TEST_BEGIN("create_beyond_max");
    setup();

    vos_handle_t h = VOS_HANDLE_INVALID;
    vos_spec_t   spec;
    uint32_t     i;

    /* Fill all slots */
    for (i = 0; i < VOS_MAX_INSTANCES; i++) {
        spec = make_spec(VOS_OS_LINUX, "fill");
        h    = VOS_HANDLE_INVALID;
        vos_create(&spec, &h);
    }

    /* One more must fail */
    spec = make_spec(VOS_OS_LINUX, "extra");
    h    = VOS_HANDLE_INVALID;
    vos_err_t rc = vos_create(&spec, &h);
    SEL4_ASSERT_EQ(rc, VOS_ERR_OUT_OF_MEMORY,
                   "create beyond max returns VOS_ERR_OUT_OF_MEMORY");
    SEL4_ASSERT_EQ((int64_t)h, (int64_t)VOS_HANDLE_INVALID,
                   "handle unchanged on failure");
}

/* ════════════════════════════════════════════════════════════════════════════════
 * Group B: CREATE → SNAPSHOT → STATUS sequences
 * ════════════════════════════════════════════════════════════════════════════════ */

static void test_snapshot_suspends_guest(void)
{
    SEL4_TEST_BEGIN("snapshot_suspends_guest");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "snap-susp");
    vos_handle_t h    = VOS_HANDLE_INVALID;
    uint32_t     lo   = 0;
    uint32_t     hi   = 0;

    vos_create(&spec, &h);

    vos_err_t rc = vos_snapshot(h, &lo, &hi);
    SEL4_ASSERT_OK(rc, "snapshot returns ok");

    vos_status_t st;
    vos_get_status(h, &st);
    SEL4_ASSERT_EQ(st.state, VOS_STATE_SUSPENDED,
                   "state is SUSPENDED after snapshot");
}

static void test_snapshot_returns_valid_token(void)
{
    SEL4_TEST_BEGIN("snapshot_returns_valid_token");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "snap-tok");
    vos_handle_t h    = VOS_HANDLE_INVALID;
    uint32_t     lo   = 0xFFFFFFFFu;
    uint32_t     hi   = 0xFFFFFFFFu;

    vos_create(&spec, &h);

    vos_err_t rc = vos_snapshot(h, &lo, &hi);
    SEL4_ASSERT_OK(rc, "snapshot ok");
    SEL4_ASSERT_TRUE(lo < VOS_SNAP_STORE_MAX,
                     "snap_lo is a valid store index");
}

static void test_snapshot_invalid_handle(void)
{
    SEL4_TEST_BEGIN("snapshot_invalid_handle");
    setup();

    uint32_t  lo = 0;
    uint32_t  hi = 0;
    vos_err_t rc = vos_snapshot(0xDEADBEEFu, &lo, &hi);
    SEL4_ASSERT_EQ(rc, VOS_ERR_INVALID_HANDLE,
                   "snapshot of non-existent handle returns VOS_ERR_INVALID_HANDLE");
}

/* ════════════════════════════════════════════════════════════════════════════════
 * Group C: CREATE → SNAPSHOT → RESTORE round-trip
 * ════════════════════════════════════════════════════════════════════════════════ */

static void test_restore_produces_running_guest(void)
{
    SEL4_TEST_BEGIN("restore_produces_running_guest");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "rt-run");
    vos_handle_t h    = VOS_HANDLE_INVALID;
    uint32_t     lo   = 0;
    uint32_t     hi   = 0;

    vos_create(&spec, &h);
    vos_snapshot(h, &lo, &hi);

    vos_handle_t h2 = VOS_HANDLE_INVALID;
    vos_err_t    rc = vos_restore(lo, hi, &spec, &h2);
    SEL4_ASSERT_OK(rc, "restore returns ok");
    SEL4_ASSERT_NE(h2, (int64_t)VOS_HANDLE_INVALID, "restore gives valid handle");

    vos_status_t st;
    vos_get_status(h2, &st);
    SEL4_ASSERT_EQ(st.state, VOS_STATE_RUNNING,
                   "restored guest state is RUNNING");
}

static void test_restore_gives_new_handle(void)
{
    SEL4_TEST_BEGIN("restore_gives_new_handle");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "rt-newhd");
    vos_handle_t h    = VOS_HANDLE_INVALID;
    uint32_t     lo   = 0;
    uint32_t     hi   = 0;

    vos_create(&spec, &h);
    vos_snapshot(h, &lo, &hi);

    vos_handle_t h2 = VOS_HANDLE_INVALID;
    vos_restore(lo, hi, &spec, &h2);

    SEL4_ASSERT_NE(h2, h, "restored handle differs from original");
    SEL4_ASSERT_NE(h2, (int64_t)VOS_HANDLE_INVALID, "restored handle is valid");
}

static void test_restore_invalid_zero_token(void)
{
    SEL4_TEST_BEGIN("restore_invalid_zero_token");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "rt-zero");
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_err_t    rc   = vos_restore(0u, 0u, &spec, &h);
    SEL4_ASSERT_EQ(rc, VOS_ERR_INVALID_HANDLE,
                   "restore with zero token returns VOS_ERR_INVALID_HANDLE");
}

static void test_restore_corrupt_token(void)
{
    SEL4_TEST_BEGIN("restore_corrupt_token");
    setup();

    /* snap index 13 has never been written */
    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "rt-crpt");
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_err_t    rc   = vos_restore(13u, 0u, &spec, &h);
    SEL4_ASSERT_EQ(rc, VOS_ERR_SNAP_NOT_FOUND,
                   "restore with empty slot returns VOS_ERR_SNAP_NOT_FOUND");
}

static void test_snapshot_restore_os_type_preserved(void)
{
    SEL4_TEST_BEGIN("snapshot_restore_os_type_preserved");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_FREEBSD, "bsd-rt");
    vos_handle_t h    = VOS_HANDLE_INVALID;
    uint32_t     lo   = 0;
    uint32_t     hi   = 0;

    vos_create(&spec, &h);
    vos_snapshot(h, &lo, &hi);

    vos_handle_t h2 = VOS_HANDLE_INVALID;
    vos_restore(lo, hi, &spec, &h2);

    vos_status_t st;
    vos_get_status(h2, &st);
    SEL4_ASSERT_EQ(st.os_type, VOS_OS_FREEBSD,
                   "restored FreeBSD guest retains VOS_OS_FREEBSD os_type");
}

/* ════════════════════════════════════════════════════════════════════════════════
 * Group D: CREATE → SNAPSHOT → DESTROY sequences
 * ════════════════════════════════════════════════════════════════════════════════ */

static void test_destroy_after_snapshot_ok(void)
{
    SEL4_TEST_BEGIN("destroy_after_snapshot_ok");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "ds-snap");
    vos_handle_t h    = VOS_HANDLE_INVALID;
    uint32_t     lo   = 0;
    uint32_t     hi   = 0;

    vos_create(&spec, &h);
    vos_snapshot(h, &lo, &hi);

    vos_err_t rc = vos_destroy(h);
    SEL4_ASSERT_OK(rc, "destroy after snapshot returns VOS_ERR_OK");
}

static void test_snapshot_of_destroyed_handle_fails(void)
{
    SEL4_TEST_BEGIN("snapshot_of_destroyed_handle_fails");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "ds-ord");
    vos_handle_t h    = VOS_HANDLE_INVALID;

    vos_create(&spec, &h);
    vos_destroy(h);

    uint32_t  lo = 0;
    uint32_t  hi = 0;
    vos_err_t rc = vos_snapshot(h, &lo, &hi);
    SEL4_ASSERT_EQ(rc, VOS_ERR_INVALID_HANDLE,
                   "snapshot of destroyed handle returns VOS_ERR_INVALID_HANDLE");
}

static void test_snapshot_persists_after_destroy(void)
{
    SEL4_TEST_BEGIN("snapshot_persists_after_destroy");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "ds-per");
    vos_handle_t h    = VOS_HANDLE_INVALID;
    uint32_t     lo   = 0;
    uint32_t     hi   = 0;

    vos_create(&spec, &h);
    vos_snapshot(h, &lo, &hi);
    vos_destroy(h);

    /* Blob must still be in the store; restore must succeed */
    vos_handle_t h2 = VOS_HANDLE_INVALID;
    vos_err_t    rc = vos_restore(lo, hi, &spec, &h2);
    SEL4_ASSERT_OK(rc, "restore from blob after original destroyed: ok");
    SEL4_ASSERT_NE(h2, (int64_t)VOS_HANDLE_INVALID, "restored handle is valid");
}

/* ════════════════════════════════════════════════════════════════════════════════
 * Group E: DESTROY sequences
 * ════════════════════════════════════════════════════════════════════════════════ */

static void test_destroy_valid_handle(void)
{
    SEL4_TEST_BEGIN("destroy_valid_handle");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "dest-ok");
    vos_handle_t h    = VOS_HANDLE_INVALID;

    vos_create(&spec, &h);

    vos_err_t rc = vos_destroy(h);
    SEL4_ASSERT_OK(rc, "destroy valid handle returns VOS_ERR_OK");
}

static void test_destroy_invalid_handle(void)
{
    SEL4_TEST_BEGIN("destroy_invalid_handle");
    setup();

    vos_err_t rc = vos_destroy(0xCAFEBABEu);
    SEL4_ASSERT_EQ(rc, VOS_ERR_INVALID_HANDLE,
                   "destroy non-existent handle returns VOS_ERR_INVALID_HANDLE");
}

static void test_destroy_same_handle_twice(void)
{
    SEL4_TEST_BEGIN("destroy_same_handle_twice");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "dest-2x");
    vos_handle_t h    = VOS_HANDLE_INVALID;

    vos_create(&spec, &h);
    vos_destroy(h);

    vos_err_t rc = vos_destroy(h);
    SEL4_ASSERT_EQ(rc, VOS_ERR_INVALID_HANDLE,
                   "second destroy of same handle returns VOS_ERR_INVALID_HANDLE");
}

/* ════════════════════════════════════════════════════════════════════════════════
 * Group F: Full lifecycle
 * ════════════════════════════════════════════════════════════════════════════════ */

static void test_full_lifecycle(void)
{
    SEL4_TEST_BEGIN("full_lifecycle");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "full");
    vos_handle_t h1   = VOS_HANDLE_INVALID;
    uint32_t     lo1  = 0;
    uint32_t     hi1  = 0;

    /* 1. Create */
    vos_err_t rc = vos_create(&spec, &h1);
    SEL4_ASSERT_OK(rc, "full_lifecycle: create ok");

    /* 2. Snapshot → h1 becomes SUSPENDED */
    rc = vos_snapshot(h1, &lo1, &hi1);
    SEL4_ASSERT_OK(rc, "full_lifecycle: first snapshot ok");

    /* 3. Restore → new handle h2 in RUNNING state */
    vos_handle_t h2 = VOS_HANDLE_INVALID;
    rc = vos_restore(lo1, hi1, &spec, &h2);
    SEL4_ASSERT_OK(rc, "full_lifecycle: first restore ok");

    /* 4. Snapshot the restored instance */
    uint32_t lo2 = 0;
    uint32_t hi2 = 0;
    rc = vos_snapshot(h2, &lo2, &hi2);
    SEL4_ASSERT_OK(rc, "full_lifecycle: second snapshot ok");

    /* 5. Restore again → h3 */
    vos_handle_t h3 = VOS_HANDLE_INVALID;
    rc = vos_restore(lo2, hi2, &spec, &h3);
    SEL4_ASSERT_OK(rc, "full_lifecycle: second restore ok");

    /* 6. Destroy all three */
    SEL4_ASSERT_OK(vos_destroy(h1), "full_lifecycle: destroy h1 ok");
    SEL4_ASSERT_OK(vos_destroy(h2), "full_lifecycle: destroy h2 ok");
    SEL4_ASSERT_OK(vos_destroy(h3), "full_lifecycle: destroy h3 ok");
}

static void test_lifecycle_slot_reuse(void)
{
    SEL4_TEST_BEGIN("lifecycle_slot_reuse");
    setup();

    vos_spec_t   spec = make_spec(VOS_OS_LINUX, "reuse");
    vos_handle_t h1   = VOS_HANDLE_INVALID;

    vos_create(&spec, &h1);
    vos_destroy(h1);

    /* Slot must have been freed; a second create must succeed */
    vos_handle_t h2 = VOS_HANDLE_INVALID;
    vos_err_t    rc = vos_create(&spec, &h2);
    SEL4_ASSERT_OK(rc, "slot_reuse: second create succeeds after destroy");
    SEL4_ASSERT_NE(h2, (int64_t)VOS_HANDLE_INVALID, "slot_reuse: second handle valid");
}

/* ════════════════════════════════════════════════════════════════════════════════
 * Group G: STATUS edge cases
 * ════════════════════════════════════════════════════════════════════════════════ */

static void test_status_invalid_handle(void)
{
    SEL4_TEST_BEGIN("status_invalid_handle");
    setup();

    vos_status_t st;
    vos_err_t    rc = vos_get_status(0xDEADu, &st);
    SEL4_ASSERT_EQ(rc, VOS_ERR_INVALID_HANDLE,
                   "status of non-existent handle returns VOS_ERR_INVALID_HANDLE");
}

/* ════════════════════════════════════════════════════════════════════════════════
 * Group H: Multi-instance isolation
 * ════════════════════════════════════════════════════════════════════════════════ */

static void test_two_guests_independent_snapshots(void)
{
    SEL4_TEST_BEGIN("two_guests_independent_snapshots");
    setup();

    vos_spec_t   specA = make_spec(VOS_OS_LINUX,   "gs-a");
    vos_spec_t   specB = make_spec(VOS_OS_FREEBSD, "gs-b");
    vos_handle_t hA    = VOS_HANDLE_INVALID;
    vos_handle_t hB    = VOS_HANDLE_INVALID;

    vos_create(&specA, &hA);
    vos_create(&specB, &hB);

    uint32_t loA = 0;
    uint32_t hiA = 0;
    uint32_t loB = 0;
    uint32_t hiB = 0;

    SEL4_ASSERT_OK(vos_snapshot(hA, &loA, &hiA), "snapshot guest A ok");
    SEL4_ASSERT_OK(vos_snapshot(hB, &loB, &hiB), "snapshot guest B ok");
    SEL4_ASSERT_NE(loA, loB, "each guest gets a distinct snap token");

    vos_handle_t hA2 = VOS_HANDLE_INVALID;
    vos_handle_t hB2 = VOS_HANDLE_INVALID;

    SEL4_ASSERT_OK(vos_restore(loA, hiA, &specA, &hA2), "restore guest A ok");
    SEL4_ASSERT_OK(vos_restore(loB, hiB, &specB, &hB2), "restore guest B ok");

    vos_status_t stA2;
    vos_status_t stB2;
    vos_get_status(hA2, &stA2);
    vos_get_status(hB2, &stB2);

    SEL4_ASSERT_EQ(stA2.state, VOS_STATE_RUNNING, "restored A is RUNNING");
    SEL4_ASSERT_EQ(stB2.state, VOS_STATE_RUNNING, "restored B is RUNNING");
    SEL4_ASSERT_EQ(stA2.os_type, VOS_OS_LINUX,   "restored A has Linux os_type");
    SEL4_ASSERT_EQ(stB2.os_type, VOS_OS_FREEBSD, "restored B has FreeBSD os_type");
}

/* ════════════════════════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    SEL4_TAP_INIT("vos_lifecycle");

    /* Group A: CREATE → STATUS */
    test_create_then_status_ok();
    test_create_sets_correct_os_type();
    test_create_freebsd_os_type();
    test_create_multiple_guests();
    test_create_beyond_max();

    /* Group B: CREATE → SNAPSHOT → STATUS */
    test_snapshot_suspends_guest();
    test_snapshot_returns_valid_token();
    test_snapshot_invalid_handle();

    /* Group C: CREATE → SNAPSHOT → RESTORE */
    test_restore_produces_running_guest();
    test_restore_gives_new_handle();
    test_restore_invalid_zero_token();
    test_restore_corrupt_token();
    test_snapshot_restore_os_type_preserved();

    /* Group D: CREATE → SNAPSHOT → DESTROY */
    test_destroy_after_snapshot_ok();
    test_snapshot_of_destroyed_handle_fails();
    test_snapshot_persists_after_destroy();

    /* Group E: DESTROY */
    test_destroy_valid_handle();
    test_destroy_invalid_handle();
    test_destroy_same_handle_twice();

    /* Group F: Full lifecycle */
    test_full_lifecycle();
    test_lifecycle_slot_reuse();

    /* Group G: STATUS edge cases */
    test_status_invalid_handle();

    /* Group H: Multi-instance isolation */
    test_two_guests_independent_snapshots();

    SEL4_TAP_FINISH();
    return SEL4_TAP_EXIT_CODE();
}
