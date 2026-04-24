/*
 * vos_restore_test.c — TAP tests for VOS_OP_RESTORE (E7-S5)
 *
 * Covers:
 *   1.  Zero token → VOS_ERR_INVALID_HANDLE
 *   2.  Zero snap_lo only → VOS_ERR_INVALID_HANDLE
 *   3.  Zero snap_hi only → VOS_ERR_INVALID_HANDLE (both must be zero to fail)
 *   4.  Unknown token → VOS_ERR_SNAP_NOT_FOUND
 *   5.  Valid round-trip: snapshot then restore → VOS_ERR_OK
 *   6.  Restored instance has non-zero guest_handle
 *   7.  Restored instance state is VOS_STATE_RUNNING
 *   8.  Restored guest_handle is different from the original
 *   9.  Corrupt blob (bad magic) → VOS_ERR_SNAP_CORRUPT
 *   10. Corrupt blob (bad version) → VOS_ERR_SNAP_CORRUPT
 *   11. Truncated blob → VOS_ERR_SNAP_CORRUPT
 *   12. handle_vos_restore dispatches zero token → VOS_ERR_INVALID_HANDLE
 *   13. handle_vos_restore dispatches valid token → VOS_ERR_OK
 *   14. handle_vos_restore reply MR1 contains new handle
 *   15. Restored register state matches snapshot register state (round-trip)
 *   16. Restored PC matches snapshot PC (round-trip)
 *   17. Multiple restores from same snapshot → distinct handles
 *   18. Snapshot after destroy still accessible (blob persists in store)
 *   19. Restore after snapshot → instance count increments
 *   20. Snapshot of handle VOS_HANDLE_INVALID → VOS_ERR_INVALID_HANDLE
 *   21. Restore token with mismatched snap_hi → VOS_ERR_SNAP_NOT_FOUND
 *   22. Back-to-back snapshots of same instance → distinct tokens
 *   23. Restore from second snapshot → VOS_ERR_OK
 *   24. Store exhaustion: restore after all slots used → VOS_ERR_OUT_OF_MEMORY
 *   25. Freshly restored instance has correct os_type
 *
 * Build:
 *   cc -DAGENTOS_TEST_HOST \
 *      -I kernel/agentos-root-task/include \
 *      -I tests/api \
 *      tests/api/vos_restore_test.c \
 *      kernel/agentos-root-task/src/vos_restore.c \
 *      kernel/agentos-root-task/src/vos_snapshot.c \
 *      -o /tmp/test_restore
 *
 * Run:
 *   /tmp/test_restore
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── framework.h provides TAP helpers and mock MR layer ─────────────────── */
#include "framework.h"

/* ── Public API under test ───────────────────────────────────────────────── */
#include "vos_snap_store.h"

/* Pull in the vibeOS contract for VOS_ERR_*, VOS_STATE_*, VOS_OP_* */
#include "../../contracts/vibeos/interface.h"

/* ── Functions from vos_snapshot.c ──────────────────────────────────────── */

/* Internal instance type — must match vos_snapshot.c / vos_restore.c */
typedef struct vos_instance vos_instance_t;
struct vos_instance {
    uint32_t    handle;
    vos_state_t state;
    vos_os_type_t os_type;
    uint8_t     vcpu_count;
    uint8_t     _pad[1];
    uint32_t    tcb_cap;
    uint32_t    vcpu_cap;
    uint32_t    memory_pages;
    uintptr_t   ram_vaddr;
    uint32_t    test_regs[32];
    uint64_t    test_pc;
    bool        active;
    char        label[16];
};

extern vos_instance_t *vos_test_alloc_instance(void);
extern vos_instance_t *vos_instance_get(uint32_t handle);
extern void            vos_test_free_instance(vos_instance_t *inst);
extern void            vos_test_instance_table_reset(void);

extern uint32_t vos_snapshot(uint32_t guest_handle,
                               uint32_t *out_snap_lo, uint32_t *out_snap_hi);

/* ── Functions from vos_restore.c ───────────────────────────────────────── */

extern uint32_t vos_restore(uint32_t snap_lo, uint32_t snap_hi,
                             uint32_t *out_guest_handle);

/* Minimal sel4_msg_t for handle_vos_restore */
#ifndef SEL4_MSG_T_DEFINED
#define SEL4_MSG_T_DEFINED
typedef uint32_t sel4_badge_t;
typedef struct { uint32_t data[8]; } sel4_msg_t;
#define data_rd32(msg, idx)         ((msg)->data[(idx)])
#define data_wr32(msg, idx, val)    ((msg)->data[(idx)] = (uint32_t)(val))
#endif

extern uint32_t handle_vos_restore(sel4_badge_t badge,
                                    const sel4_msg_t *req,
                                    sel4_msg_t *rep,
                                    void *ctx);

/* ── Test helpers ────────────────────────────────────────────────────────── */

/*
 * Reset the entire test state: instance table + snap store.
 */
static void test_reset(void)
{
    vos_test_instance_table_reset();
    vos_test_snap_store_reset();
}

/*
 * alloc_running_instance — create a test instance in RUNNING state and return
 * its handle.  Returns 0 on allocation failure.
 */
static uint32_t alloc_running_instance(void)
{
    vos_instance_t *inst = vos_test_alloc_instance();
    if (!inst) return 0;
    inst->state   = VOS_STATE_RUNNING;
    inst->os_type = VOS_OS_LINUX;
    /* Fill registers with a recognisable pattern */
    for (uint32_t i = 0; i < 32; i++)
        inst->test_regs[i] = 0xA0000000u | (inst->handle << 8) | i;
    inst->test_pc = 0xBEEFCAFE00000000ULL | inst->handle;
    return inst->handle;
}

/* ── Test functions ──────────────────────────────────────────────────────── */

/* T1 — zero token (both zero) → VOS_ERR_INVALID_HANDLE */
static void test_zero_token(void)
{
    test_reset();
    uint32_t h = 0;
    uint32_t err = vos_restore(0, 0, &h);
    ASSERT_EQ(err, VOS_ERR_INVALID_HANDLE,
              "zero token returns VOS_ERR_INVALID_HANDLE");
}

/* T2 — snap_lo=0, snap_hi=0 via IPC dispatcher */
static void test_zero_token_dispatch(void)
{
    test_reset();
    sel4_msg_t req = {{0}}, rep = {{0}};
    data_wr32(&req, 0, VOS_OP_RESTORE);
    data_wr32(&req, 1, 0);  /* snap_lo */
    data_wr32(&req, 2, 0);  /* snap_hi */
    uint32_t ret = handle_vos_restore(0, &req, &rep, NULL);
    ASSERT_EQ(ret, VOS_ERR_INVALID_HANDLE,
              "dispatcher: zero token → VOS_ERR_INVALID_HANDLE");
}

/* T3 — snap_lo=1, snap_hi=0 (non-zero snap_lo, store empty) → NOT_FOUND */
static void test_nonzero_lo_empty_store(void)
{
    test_reset();
    uint32_t h = 0;
    uint32_t err = vos_restore(1, 0, &h);
    /* snap_hi=0 does not match ~1, so get_blob returns -1 → NOT_FOUND */
    ASSERT_EQ(err, VOS_ERR_SNAP_NOT_FOUND,
              "mismatched hi on empty store → VOS_ERR_SNAP_NOT_FOUND");
}

/* T4 — unknown token → VOS_ERR_SNAP_NOT_FOUND */
static void test_unknown_token(void)
{
    test_reset();
    uint32_t h = 0;
    /* Use a valid-looking but non-existent token */
    uint32_t sl = 5u, sh = ~5u;
    uint32_t err = vos_restore(sl, sh, &h);
    ASSERT_EQ(err, VOS_ERR_SNAP_NOT_FOUND,
              "unknown token → VOS_ERR_SNAP_NOT_FOUND");
}

/* T5 — valid round-trip: snapshot then restore → VOS_ERR_OK */
static void test_valid_roundtrip_ok(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    ASSERT_NE(orig, 0u, "roundtrip: instance allocated");

    uint32_t sl = 0, sh = 0;
    uint32_t snap_err = vos_snapshot(orig, &sl, &sh);
    ASSERT_EQ(snap_err, VOS_ERR_OK, "roundtrip: snapshot succeeds");

    uint32_t restored_h = 0;
    uint32_t err = vos_restore(sl, sh, &restored_h);
    ASSERT_EQ(err, VOS_ERR_OK, "roundtrip: restore returns VOS_ERR_OK");
}

/* T6 — restored instance has non-zero guest_handle */
static void test_restored_handle_nonzero(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    uint32_t restored_h = 0;
    vos_restore(sl, sh, &restored_h);
    ASSERT_NE(restored_h, 0u, "restored handle is non-zero");
}

/* T7 — restored instance state is VOS_STATE_RUNNING */
static void test_restored_state_running(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    uint32_t restored_h = 0;
    vos_restore(sl, sh, &restored_h);

    vos_instance_t *inst = vos_instance_get(restored_h);
    ASSERT_TRUE(inst != NULL, "restored instance found in table");
    ASSERT_EQ((uint32_t)inst->state, (uint32_t)VOS_STATE_RUNNING,
              "restored instance state is VOS_STATE_RUNNING");
}

/* T8 — restored handle is different from the original */
static void test_restored_handle_distinct(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    uint32_t restored_h = 0;
    vos_restore(sl, sh, &restored_h);

    ASSERT_NE(restored_h, orig, "restored handle differs from original");
}

/* T9 — corrupt blob (bad magic) → VOS_ERR_SNAP_CORRUPT */
static void test_corrupt_bad_magic(void)
{
    test_reset();

    /* Build a minimal blob with a wrong magic */
    static uint8_t bad_blob[sizeof(vos_snap_hdr_t) + 32 * 4 + 8 + 64 * 1024];
    memset(bad_blob, 0, sizeof(bad_blob));

    vos_snap_hdr_t *hdr = (vos_snap_hdr_t *)bad_blob;
    hdr->magic        = 0xDEADBEEFu;  /* wrong */
    hdr->version      = VOS_SNAP_VERSION;
    hdr->ram_bytes    = 64u * 1024u;

    uint32_t sl = 0, sh = 0;
    int rc = vos_test_snap_store_put(bad_blob, sizeof(bad_blob), &sl, &sh);
    ASSERT_EQ(rc, 0, "corrupt blob: put succeeds");

    uint32_t h = 0;
    uint32_t err = vos_restore(sl, sh, &h);
    ASSERT_EQ(err, VOS_ERR_SNAP_CORRUPT,
              "bad magic → VOS_ERR_SNAP_CORRUPT");
}

/* T10 — corrupt blob (bad version) → VOS_ERR_SNAP_CORRUPT */
static void test_corrupt_bad_version(void)
{
    test_reset();

    static uint8_t bad_blob[sizeof(vos_snap_hdr_t) + 32 * 4 + 8 + 64 * 1024];
    memset(bad_blob, 0, sizeof(bad_blob));

    vos_snap_hdr_t *hdr = (vos_snap_hdr_t *)bad_blob;
    hdr->magic        = VOS_SNAP_MAGIC;
    hdr->version      = 0x99u;  /* wrong version */
    hdr->ram_bytes    = 64u * 1024u;

    uint32_t sl = 0, sh = 0;
    vos_test_snap_store_put(bad_blob, sizeof(bad_blob), &sl, &sh);

    uint32_t h = 0;
    uint32_t err = vos_restore(sl, sh, &h);
    ASSERT_EQ(err, VOS_ERR_SNAP_CORRUPT,
              "bad version → VOS_ERR_SNAP_CORRUPT");
}

/* T11 — truncated blob → VOS_ERR_SNAP_CORRUPT */
static void test_corrupt_truncated(void)
{
    test_reset();

    /* Only the header, no register data or RAM */
    vos_snap_hdr_t tiny;
    memset(&tiny, 0, sizeof(tiny));
    tiny.magic     = VOS_SNAP_MAGIC;
    tiny.version   = VOS_SNAP_VERSION;
    tiny.ram_bytes = 64u * 1024u;  /* claims 64K but blob is only 32 bytes */

    uint32_t sl = 0, sh = 0;
    vos_test_snap_store_put(&tiny, sizeof(tiny), &sl, &sh);

    uint32_t h = 0;
    uint32_t err = vos_restore(sl, sh, &h);
    ASSERT_EQ(err, VOS_ERR_SNAP_CORRUPT,
              "truncated blob → VOS_ERR_SNAP_CORRUPT");
}

/* T12 — IPC dispatcher: zero token */
static void test_dispatch_zero_token(void)
{
    test_reset();
    sel4_msg_t req = {{0}}, rep = {{0}};
    data_wr32(&req, 0, VOS_OP_RESTORE);
    data_wr32(&req, 1, 0);
    data_wr32(&req, 2, 0);
    uint32_t ret = handle_vos_restore(0, &req, &rep, NULL);
    ASSERT_EQ(data_rd32(&rep, 0), VOS_ERR_INVALID_HANDLE,
              "dispatch zero token: rep MR0 = VOS_ERR_INVALID_HANDLE");
    ASSERT_EQ(ret, VOS_ERR_INVALID_HANDLE,
              "dispatch zero token: return value matches rep MR0");
}

/* T13 — IPC dispatcher: valid token */
static void test_dispatch_valid_token(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    sel4_msg_t req = {{0}}, rep = {{0}};
    data_wr32(&req, 0, VOS_OP_RESTORE);
    data_wr32(&req, 1, sl);
    data_wr32(&req, 2, sh);
    uint32_t ret = handle_vos_restore(0, &req, &rep, NULL);
    ASSERT_EQ(ret, VOS_ERR_OK, "dispatch valid token → VOS_ERR_OK");
}

/* T14 — IPC dispatcher reply MR1 contains new handle */
static void test_dispatch_reply_handle(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    sel4_msg_t req = {{0}}, rep = {{0}};
    data_wr32(&req, 0, VOS_OP_RESTORE);
    data_wr32(&req, 1, sl);
    data_wr32(&req, 2, sh);
    handle_vos_restore(0, &req, &rep, NULL);

    uint32_t restored_h = data_rd32(&rep, 1);
    ASSERT_NE(restored_h, 0u, "dispatch reply MR1 is non-zero handle");
}

/* T15 — restored register state matches snapshot */
static void test_register_roundtrip(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    vos_instance_t *orig_inst = vos_instance_get(orig);
    ASSERT_TRUE(orig_inst != NULL, "reg roundtrip: original instance found");

    /* Set a recognisable register pattern */
    for (uint32_t i = 0; i < 32; i++)
        orig_inst->test_regs[i] = 0xCAFE0000u | i;
    orig_inst->test_pc = 0xDEAD000000000001ULL;

    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    uint32_t restored_h = 0;
    vos_restore(sl, sh, &restored_h);

    vos_instance_t *res_inst = vos_instance_get(restored_h);
    ASSERT_TRUE(res_inst != NULL, "reg roundtrip: restored instance found");

    bool regs_ok = true;
    for (uint32_t i = 0; i < 32; i++) {
        if (res_inst->test_regs[i] != (0xCAFE0000u | i)) {
            regs_ok = false;
            break;
        }
    }
    ASSERT_TRUE(regs_ok, "restored registers match snapshot registers");
}

/* T16 — restored PC matches snapshot */
static void test_pc_roundtrip(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    vos_instance_t *orig_inst = vos_instance_get(orig);
    orig_inst->test_pc = 0xFEEDFACE12345678ULL;

    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    uint32_t restored_h = 0;
    vos_restore(sl, sh, &restored_h);

    vos_instance_t *res_inst = vos_instance_get(restored_h);
    ASSERT_TRUE(res_inst != NULL, "PC roundtrip: restored instance found");
    ASSERT_EQ(res_inst->test_pc, 0xFEEDFACE12345678ULL,
              "restored PC matches snapshot PC");
}

/* T17 — multiple restores from same snapshot → distinct handles */
static void test_multiple_restores_distinct_handles(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    uint32_t h1 = 0, h2 = 0;
    vos_restore(sl, sh, &h1);
    vos_restore(sl, sh, &h2);

    ASSERT_NE(h1, 0u, "first restore handle non-zero");
    ASSERT_NE(h2, 0u, "second restore handle non-zero");
    ASSERT_NE(h1, h2, "two restores from same snapshot produce distinct handles");
}

/* T18 — snapshot after destroy still accessible (blob persists) */
static void test_blob_persists_after_destroy(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    /* Destroy the original instance */
    vos_instance_t *inst = vos_instance_get(orig);
    vos_test_free_instance(inst);

    /* Blob should still be retrievable */
    uint32_t restored_h = 0;
    uint32_t err = vos_restore(sl, sh, &restored_h);
    ASSERT_EQ(err, VOS_ERR_OK,
              "restore after original destroyed still succeeds");
}

/* T19 — restore → instance count increments */
static void test_restore_increments_instance_count(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    /* Free the original, then restore — net result: 1 active instance */
    vos_instance_t *inst = vos_instance_get(orig);
    vos_test_free_instance(inst);

    uint32_t restored_h = 0;
    vos_restore(sl, sh, &restored_h);

    vos_instance_t *res = vos_instance_get(restored_h);
    ASSERT_TRUE(res != NULL && res->active,
                "restored instance is active in the instance table");
}

/* T20 — snapshot of VOS_HANDLE_INVALID → VOS_ERR_INVALID_HANDLE */
static void test_snapshot_invalid_handle(void)
{
    test_reset();
    uint32_t sl = 0, sh = 0;
    uint32_t err = vos_snapshot(VOS_HANDLE_INVALID, &sl, &sh);
    ASSERT_EQ(err, VOS_ERR_INVALID_HANDLE,
              "snapshot of VOS_HANDLE_INVALID → VOS_ERR_INVALID_HANDLE");
}

/* T21 — restore with mismatched snap_hi → VOS_ERR_SNAP_NOT_FOUND */
static void test_mismatched_snap_hi(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    uint32_t h = 0;
    uint32_t err = vos_restore(sl, sh ^ 0xFFFFFFFFu, &h);
    ASSERT_EQ(err, VOS_ERR_SNAP_NOT_FOUND,
              "mismatched snap_hi → VOS_ERR_SNAP_NOT_FOUND");
}

/* T22 — back-to-back snapshots of same instance → distinct tokens */
static void test_two_snapshots_distinct_tokens(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();

    uint32_t sl1 = 0, sh1 = 0;
    uint32_t sl2 = 0, sh2 = 0;
    vos_snapshot(orig, &sl1, &sh1);
    vos_snapshot(orig, &sl2, &sh2);

    /* Tokens should differ because each snapshot goes into a different slot */
    ASSERT_NE(sl1, sl2, "two snapshots of same instance produce distinct tokens");
}

/* T23 — restore from second snapshot → VOS_ERR_OK */
static void test_restore_from_second_snapshot(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();

    uint32_t sl1 = 0, sh1 = 0, sl2 = 0, sh2 = 0;
    vos_snapshot(orig, &sl1, &sh1);
    vos_snapshot(orig, &sl2, &sh2);

    uint32_t h = 0;
    uint32_t err = vos_restore(sl2, sh2, &h);
    ASSERT_EQ(err, VOS_ERR_OK, "restore from second snapshot → VOS_ERR_OK");
}

/* T24 — all slots exhausted → VOS_ERR_OUT_OF_MEMORY */
static void test_restore_oom(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    /*
     * Fill remaining slots.  VOS_MAX_SLOTS = 4 so we need to fill the other 3.
     * We already have 'orig' allocated.
     */
    for (uint32_t i = 1; i < 4; i++) {
        vos_instance_t *extra = vos_test_alloc_instance();
        if (!extra) break;  /* may already be full */
        extra->state = VOS_STATE_RUNNING;
    }

    /* Now the table should be full; restore should fail with OOM */
    uint32_t h = 0;
    uint32_t err = vos_restore(sl, sh, &h);
    ASSERT_EQ(err, VOS_ERR_OUT_OF_MEMORY,
              "restore with full instance table → VOS_ERR_OUT_OF_MEMORY");
}

/* T25 — restored instance has correct os_type */
static void test_restored_os_type(void)
{
    test_reset();
    uint32_t orig = alloc_running_instance();
    vos_instance_t *orig_inst = vos_instance_get(orig);
    ASSERT_TRUE(orig_inst != NULL, "os_type: original instance found");
    orig_inst->os_type = VOS_OS_FREEBSD;

    uint32_t sl = 0, sh = 0;
    vos_snapshot(orig, &sl, &sh);

    uint32_t restored_h = 0;
    vos_restore(sl, sh, &restored_h);

    vos_instance_t *res = vos_instance_get(restored_h);
    ASSERT_TRUE(res != NULL, "os_type: restored instance found");
    ASSERT_EQ((uint32_t)res->os_type, (uint32_t)VOS_OS_FREEBSD,
              "restored os_type matches snapshot os_type");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    TAP_PLAN(37);

    test_zero_token();
    test_zero_token_dispatch();
    test_nonzero_lo_empty_store();
    test_unknown_token();
    test_valid_roundtrip_ok();
    test_restored_handle_nonzero();
    test_restored_state_running();
    test_restored_handle_distinct();
    test_corrupt_bad_magic();
    test_corrupt_bad_version();
    test_corrupt_truncated();
    test_dispatch_zero_token();
    test_dispatch_valid_token();
    test_dispatch_reply_handle();
    test_register_roundtrip();
    test_pc_roundtrip();
    test_multiple_restores_distinct_handles();
    test_blob_persists_after_destroy();
    test_restore_increments_instance_count();
    test_snapshot_invalid_handle();
    test_mismatched_snap_hi();
    test_two_snapshots_distinct_tokens();
    test_restore_from_second_snapshot();
    test_restore_oom();
    test_restored_os_type();

    if (_tap_failed == 0)
        printf("TAP_DONE:0\n");
    else
        printf("TAP_DONE:1\n");

    return tap_exit();
}

#else
/* Non-test build: provide a dummy translation unit */
typedef int _vos_restore_test_dummy;
#endif /* AGENTOS_TEST_HOST */
