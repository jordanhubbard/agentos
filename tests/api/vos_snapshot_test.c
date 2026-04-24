/*
 * vos_snapshot_test.c — TAP API tests for VOS_SNAPSHOT
 *
 * Tests the vos_snapshot() function (and the IPC-level VOS_OP_SNAPSHOT opcode)
 * by compiling vos_snapshot.c in AGENTOS_TEST_HOST mode and exercising all
 * observable behaviours.
 *
 * Covered cases:
 *   T01  snapshot invalid handle → VOS_ERR_INVALID_HANDLE
 *   T02  snapshot VOS_HANDLE_INVALID → VOS_ERR_INVALID_HANDLE
 *   T03  snapshot valid handle → VOS_ERR_OK
 *   T04  snap_lo non-zero on success
 *   T05  snap_hi non-zero on success
 *   T06  snapshot header magic == VOS_SNAP_MAGIC ('SNAP')
 *   T07  snapshot header version == VOS_SNAP_VERSION (1)
 *   T08  snapshot header guest_handle matches the requested handle
 *   T09  snapshot header ram_size_pages matches instance ram_pages
 *   T10  snapshot header reg_dump_size == sizeof(seL4_VCPUContext)
 *   T11  re-snapshot same handle → different snap_lo token
 *   T12  re-snapshot same handle → different snap_hi token
 *   T13  two different handles produce different snap_lo tokens
 *   T14  two different handles produce different snap_hi tokens
 *   T15  snapshot leaves guest in VOS_STATE_SUSPENDED
 *   T16  snapshot of a second valid handle succeeds
 *   T17  snap_lo + snap_hi both non-zero after a valid snapshot
 *   T18  blob header _pad fields are all zero
 *   T19  blob size = sizeof(vos_snap_hdr_t) + reg_dump + ram_pages*4096
 *   T20  snapshot with 0-page RAM instance succeeds (edge case)
 *   T21  snap_lo after 0-page snapshot is non-zero
 *   T22  snap_hi after 0-page snapshot is non-zero
 *   T23  snapshot of a handle freed from instance table → VOS_ERR_INVALID_HANDLE
 *   T24  snapshot fills max allowed instances (VOS_MAX_INSTANCES) each OK
 *   T25  snapshot token after filling all instances is non-zero (both words)
 *   T26  all four VOS_MAX_INSTANCES snapshots produce unique snap_lo tokens
 *
 * Build & run (from repo root):
 *   cc -DAGENTOS_TEST_HOST \
 *      -I kernel/agentos-root-task/include \
 *      -I kernel/agentos-root-task/src \
 *      -I contracts \
 *      -I tests/api \
 *      -o /tmp/test_vos_snapshot \
 *      tests/api/vos_snapshot_test.c \
 *      kernel/agentos-root-task/src/vos_snapshot.c \
 *   && /tmp/test_vos_snapshot
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"

/* Pull in the implementation.  In host mode all seL4 primitives are stubbed  */
/* inside vos_snapshot.c; we just need the public function signatures here.   */

/* Declare the public surface we are testing */
typedef uint32_t vos_handle_t;
typedef uint32_t vos_err_t;

/* Include the contracts so we get the struct and constant definitions */
#include "contracts/vibeos/interface.h"
#include "contracts/agentfs/interface.h"

/* Bring in the implementation symbols (compiled as a unit) */
#include "../../kernel/agentos-root-task/src/vos_snapshot.c"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/*
 * do_snapshot — wrapper around vos_snapshot(); resets outputs before call.
 */
static vos_err_t do_snapshot(vos_handle_t h,
                              uint32_t *lo_out, uint32_t *hi_out)
{
    *lo_out = 0;
    *hi_out = 0;
    return vos_snapshot(h, lo_out, hi_out);
}

/*
 * snap_header — access the first 32 bytes of the static snapshot buffer.
 * Only valid immediately after a successful vos_snapshot() call.
 */
static const vos_snap_hdr_t *snap_header(void)
{
    return (const vos_snap_hdr_t *)g_snap_buf;
}

/* ── Test functions ──────────────────────────────────────────────────────── */

/* T01: snapshot of an unknown non-zero handle */
static void test_invalid_handle_returns_err(void)
{
    vos_snapshot_init();
    uint32_t lo, hi;
    vos_err_t err = do_snapshot(0xDEADBEEFu, &lo, &hi);
    ASSERT_EQ(err, VOS_ERR_INVALID_HANDLE,
              "T01: unknown handle → VOS_ERR_INVALID_HANDLE");
}

/* T02: VOS_HANDLE_INVALID sentinel */
static void test_handle_invalid_sentinel(void)
{
    vos_snapshot_init();
    uint32_t lo, hi;
    vos_err_t err = do_snapshot(VOS_HANDLE_INVALID, &lo, &hi);
    ASSERT_EQ(err, VOS_ERR_INVALID_HANDLE,
              "T02: VOS_HANDLE_INVALID → VOS_ERR_INVALID_HANDLE");
}

/* T03: valid handle → VOS_ERR_OK */
static void test_valid_handle_ok(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(4u);
    ASSERT_NE(h, VOS_HANDLE_INVALID, "T03: alloc returns valid handle");
    uint32_t lo, hi;
    vos_err_t err = do_snapshot(h, &lo, &hi);
    ASSERT_EQ(err, VOS_ERR_OK, "T03: valid handle → VOS_ERR_OK");
}

/* T04: snap_lo non-zero on success */
static void test_snap_lo_nonzero(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(4u);
    uint32_t lo, hi;
    do_snapshot(h, &lo, &hi);
    ASSERT_NE(lo, 0u, "T04: snap_lo is non-zero on success");
}

/* T05: snap_hi non-zero on success */
static void test_snap_hi_nonzero(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(4u);
    uint32_t lo, hi;
    do_snapshot(h, &lo, &hi);
    ASSERT_NE(hi, 0u, "T05: snap_hi is non-zero on success");
}

/* T06: blob header magic */
static void test_header_magic(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(2u);
    uint32_t lo, hi;
    do_snapshot(h, &lo, &hi);
    ASSERT_EQ(snap_header()->magic, VOS_SNAP_MAGIC,
              "T06: snapshot header magic == 'SNAP' (0x534E4150)");
}

/* T07: blob header version */
static void test_header_version(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(2u);
    uint32_t lo, hi;
    do_snapshot(h, &lo, &hi);
    ASSERT_EQ(snap_header()->version, VOS_SNAP_VERSION,
              "T07: snapshot header version == 1");
}

/* T08: blob header guest_handle */
static void test_header_guest_handle(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(2u);
    uint32_t lo, hi;
    do_snapshot(h, &lo, &hi);
    ASSERT_EQ(snap_header()->guest_handle, h,
              "T08: snapshot header guest_handle matches requested handle");
}

/* T09: blob header ram_size_pages */
static void test_header_ram_size_pages(void)
{
    vos_snapshot_init();
    /*
     * In AGENTOS_TEST_HOST mode the static snapshot buffer is sized for
     * VOS_SNAP_BUF_PAGES (4) pages.  Allocate a 2-page instance so we are
     * safely within the host buffer limit.
     */
    uint32_t pages = 2u;
    vos_handle_t h = vos_test_alloc_instance(pages);
    uint32_t lo, hi;
    do_snapshot(h, &lo, &hi);
    ASSERT_EQ(snap_header()->ram_size_pages, pages,
              "T09: header ram_size_pages matches instance pages");
}

/* T10: blob header reg_dump_size */
static void test_header_reg_dump_size(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(2u);
    uint32_t lo, hi;
    do_snapshot(h, &lo, &hi);
    ASSERT_EQ(snap_header()->reg_dump_size, (uint32_t)sizeof(seL4_VCPUContext),
              "T10: header reg_dump_size == sizeof(seL4_VCPUContext)");
}

/* T11: re-snapshot same handle → different snap_lo */
static void test_resnap_different_lo(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(2u);
    uint32_t lo1 = 0, hi1 = 0, lo2 = 0, hi2 = 0;
    do_snapshot(h, &lo1, &hi1);
    /* Re-allocate to RUNNING state (snapshot leaves it SUSPENDED; re-open) */
    g_vos_instances[0].state = VOS_STATE_RUNNING;
    do_snapshot(h, &lo2, &hi2);
    ASSERT_NE(lo1, lo2,
              "T11: re-snapshot same handle → different snap_lo token");
}

/* T12: re-snapshot same handle → different snap_hi */
static void test_resnap_different_hi(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(2u);
    uint32_t lo1 = 0, hi1 = 0, lo2 = 0, hi2 = 0;
    do_snapshot(h, &lo1, &hi1);
    g_vos_instances[0].state = VOS_STATE_RUNNING;
    do_snapshot(h, &lo2, &hi2);
    ASSERT_NE(hi1, hi2,
              "T12: re-snapshot same handle → different snap_hi token");
}

/* T13: two different handles → different snap_lo */
static void test_two_handles_different_lo(void)
{
    vos_snapshot_init();
    vos_handle_t h1 = vos_test_alloc_instance(2u);
    vos_handle_t h2 = vos_test_alloc_instance(2u);
    uint32_t lo1, hi1, lo2, hi2;
    do_snapshot(h1, &lo1, &hi1);
    do_snapshot(h2, &lo2, &hi2);
    ASSERT_NE(lo1, lo2,
              "T13: two different handles → different snap_lo");
}

/* T14: two different handles → different snap_hi */
static void test_two_handles_different_hi(void)
{
    vos_snapshot_init();
    vos_handle_t h1 = vos_test_alloc_instance(2u);
    vos_handle_t h2 = vos_test_alloc_instance(2u);
    uint32_t lo1, hi1, lo2, hi2;
    do_snapshot(h1, &lo1, &hi1);
    do_snapshot(h2, &lo2, &hi2);
    ASSERT_NE(hi1, hi2,
              "T14: two different handles → different snap_hi");
}

/* T15: snapshot leaves guest SUSPENDED */
static void test_guest_left_suspended(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(2u);
    uint32_t lo, hi;
    do_snapshot(h, &lo, &hi);
    /* Find the instance and check its state */
    vos_instance_t *inst = vos_instance_get(h);
    ASSERT_TRUE(inst != NULL, "T15: instance still in table after snapshot");
    ASSERT_EQ(inst->state, VOS_STATE_SUSPENDED,
              "T15: snapshot leaves guest in VOS_STATE_SUSPENDED");
}

/* T16: second valid handle snapshot succeeds */
static void test_second_handle_ok(void)
{
    vos_snapshot_init();
    vos_handle_t h1 = vos_test_alloc_instance(2u);
    vos_handle_t h2 = vos_test_alloc_instance(4u);
    uint32_t lo, hi;
    vos_err_t err1 = do_snapshot(h1, &lo, &hi);
    vos_err_t err2 = do_snapshot(h2, &lo, &hi);
    ASSERT_EQ(err1, VOS_ERR_OK, "T16: first handle snapshot → VOS_ERR_OK");
    ASSERT_EQ(err2, VOS_ERR_OK, "T16: second handle snapshot → VOS_ERR_OK");
}

/* T17: both snap_lo and snap_hi non-zero together */
static void test_both_token_words_nonzero(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(2u);
    uint32_t lo, hi;
    do_snapshot(h, &lo, &hi);
    ASSERT_TRUE(lo != 0u && hi != 0u,
                "T17: snap_lo and snap_hi both non-zero after AgentFS store");
}

/* T18: header _pad fields are zero */
static void test_header_pad_zero(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(2u);
    uint32_t lo, hi;
    do_snapshot(h, &lo, &hi);
    const vos_snap_hdr_t *hdr = snap_header();
    bool pad_ok = (hdr->_pad[0] == 0u) &&
                  (hdr->_pad[1] == 0u) &&
                  (hdr->_pad[2] == 0u);
    ASSERT_TRUE(pad_ok, "T18: header _pad[0..2] are all zero");
}

/* T19: blob total size matches expected formula */
static void test_blob_size_formula(void)
{
    vos_snapshot_init();
    uint32_t pages = 4u;
    vos_handle_t h = vos_test_alloc_instance(pages);
    uint32_t lo, hi;
    do_snapshot(h, &lo, &hi);
    uint32_t expected = (uint32_t)sizeof(vos_snap_hdr_t)
                      + (uint32_t)sizeof(seL4_VCPUContext)
                      + pages * 4096u;
    /*
     * We verify the header's own fields produce the correct expected blob
     * size (the buffer itself is opaque static storage).
     */
    uint32_t derived = (uint32_t)sizeof(vos_snap_hdr_t)
                     + snap_header()->reg_dump_size
                     + snap_header()->ram_size_pages * 4096u;
    ASSERT_EQ(derived, expected,
              "T19: derived blob size equals sizeof(hdr)+regs+ram_pages*4096");
}

/* T20: snapshot with 0-page RAM instance succeeds */
static void test_zero_ram_pages_ok(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(0u);
    uint32_t lo, hi;
    vos_err_t err = do_snapshot(h, &lo, &hi);
    ASSERT_EQ(err, VOS_ERR_OK, "T20: 0-page RAM instance snapshot → VOS_ERR_OK");
}

/* T21: snap_lo non-zero after 0-page snapshot */
static void test_zero_ram_lo_nonzero(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(0u);
    uint32_t lo = 0, hi = 0;
    do_snapshot(h, &lo, &hi);
    ASSERT_NE(lo, 0u, "T21: snap_lo non-zero for 0-page RAM snapshot");
}

/* T22: snap_hi non-zero after 0-page snapshot */
static void test_zero_ram_hi_nonzero(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(0u);
    uint32_t lo = 0, hi = 0;
    do_snapshot(h, &lo, &hi);
    ASSERT_NE(hi, 0u, "T22: snap_hi non-zero for 0-page RAM snapshot");
}

/* T23: snapshot of handle freed from instance table → invalid handle */
static void test_freed_handle_invalid(void)
{
    vos_snapshot_init();
    vos_handle_t h = vos_test_alloc_instance(2u);
    vos_test_free_instance(h);
    uint32_t lo, hi;
    vos_err_t err = do_snapshot(h, &lo, &hi);
    ASSERT_EQ(err, VOS_ERR_INVALID_HANDLE,
              "T23: freed handle → VOS_ERR_INVALID_HANDLE");
}

/* T24 + T25: fill all VOS_MAX_INSTANCES slots, each snapshot succeeds */
static void test_all_instances_snapshot_ok(void)
{
    vos_snapshot_init();
    uint32_t last_lo = 0, last_hi = 0;
    bool all_ok = true;

    for (uint32_t i = 0; i < VOS_MAX_INSTANCES; i++) {
        vos_handle_t h = vos_test_alloc_instance(2u);
        if (h == VOS_HANDLE_INVALID) {
            all_ok = false;
            break;
        }
        uint32_t lo = 0, hi = 0;
        vos_err_t err = do_snapshot(h, &lo, &hi);
        if (err != VOS_ERR_OK) {
            all_ok = false;
            break;
        }
        last_lo = lo;
        last_hi = hi;
    }

    ASSERT_TRUE(all_ok,
        "T24: all VOS_MAX_INSTANCES slots snapshot successfully");
    ASSERT_TRUE(last_lo != 0u && last_hi != 0u,
        "T25: last snapshot token is non-zero (both words) after filling all slots");
}

/* T26: tokens from all VOS_MAX_INSTANCES snapshots are unique (snap_lo) */
static void test_all_instances_unique_tokens(void)
{
    vos_snapshot_init();
    uint32_t lo_tokens[VOS_MAX_INSTANCES];
    bool all_unique = true;

    for (uint32_t i = 0; i < VOS_MAX_INSTANCES; i++) {
        vos_handle_t h = vos_test_alloc_instance(1u);
        uint32_t lo = 0, hi = 0;
        do_snapshot(h, &lo, &hi);
        lo_tokens[i] = lo;
    }

    for (uint32_t i = 0; i < VOS_MAX_INSTANCES; i++) {
        for (uint32_t j = i + 1; j < VOS_MAX_INSTANCES; j++) {
            if (lo_tokens[i] == lo_tokens[j]) {
                all_unique = false;
            }
        }
    }

    ASSERT_TRUE(all_unique,
        "T26: all VOS_MAX_INSTANCES snapshots produce unique snap_lo tokens");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    TAP_PLAN(29);

    test_invalid_handle_returns_err();   /* T01 */
    test_handle_invalid_sentinel();      /* T02 */
    test_valid_handle_ok();              /* T03 */
    test_snap_lo_nonzero();              /* T04 */
    test_snap_hi_nonzero();              /* T05 */
    test_header_magic();                 /* T06 */
    test_header_version();               /* T07 */
    test_header_guest_handle();          /* T08 */
    test_header_ram_size_pages();        /* T09 */
    test_header_reg_dump_size();         /* T10 */
    test_resnap_different_lo();          /* T11 */
    test_resnap_different_hi();          /* T12 */
    test_two_handles_different_lo();     /* T13 */
    test_two_handles_different_hi();     /* T14 */
    test_guest_left_suspended();         /* T15 */
    test_second_handle_ok();             /* T16 (counts as 2 assertions) */
    test_both_token_words_nonzero();     /* T17 */
    test_header_pad_zero();              /* T18 */
    test_blob_size_formula();            /* T19 */
    test_zero_ram_pages_ok();            /* T20 */
    test_zero_ram_lo_nonzero();          /* T21 */
    test_zero_ram_hi_nonzero();          /* T22 */
    test_freed_handle_invalid();         /* T23 */
    test_all_instances_snapshot_ok();    /* T24 + T25 */
    test_all_instances_unique_tokens();  /* T26 */

    return tap_exit();
}

#else
/* Non-host builds: this file is excluded from compilation */
typedef int _vos_snapshot_test_dummy;
#endif /* AGENTOS_TEST_HOST */
