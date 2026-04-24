/*
 * test_log_drain.c — API tests for the agentOS log_drain PD
 *
 * Covered opcodes and scenarios:
 *   OP_LOG_WRITE  (0x87) — register ring + drain
 *   OP_LOG_STATUS (0x86) — return ring_count + total_bytes
 *   unknown opcode       — must return SEL4_ERR_INVALID_OP
 *
 * Tests pull in the log_drain implementation directly under
 * -DAGENTOS_TEST_HOST so no seL4 is required.  All ring I/O uses a
 * statically allocated buffer in BSS; serial output stubs are no-ops.
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST \
 *      -I tests/api \
 *      -I kernel/agentos-root-task/include \
 *      -o /tmp/test_log_drain \
 *      tests/api/test_log_drain.c && /tmp/test_log_drain
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"

/* Pull in the PD implementation.  The AGENTOS_TEST_HOST guard inside
 * log_drain.c replaces all seL4/Microkit references with stubs. */
#include "../../kernel/agentos-root-task/src/log_drain.c"

/* ── Ring buffer backing store ──────────────────────────────────────────────── */

/*
 * Each ring slot is RING_SIZE (4096) bytes.  MAX_LOG_RINGS == 16, so the
 * total backing store is 64 KB — fits in BSS with no issues.
 */
#define TEST_RING_COUNT  16
#define TEST_RING_TOTAL  (TEST_RING_COUNT * RING_SIZE)

static uint8_t ring_backing[TEST_RING_TOTAL];

/* ── Setup helpers ──────────────────────────────────────────────────────────── */

static void setup(void) {
    memset(ring_backing, 0, sizeof(ring_backing));
    log_drain_rings_vaddr = (uintptr_t)ring_backing;
    serial_shmem_vaddr    = 0;   /* no serial_pd in tests */
    log_drain_test_init();
}

/* ── Low-level helpers (match nameserver.c data_rd/wr style) ──────────────── */

static inline uint32_t data_rd32_t(const uint8_t *d, int off) {
    return (uint32_t)d[off    ]
         | ((uint32_t)d[off+1] <<  8)
         | ((uint32_t)d[off+2] << 16)
         | ((uint32_t)d[off+3] << 24);
}
static inline void data_wr32_t(uint8_t *d, int off, uint32_t v) {
    d[off  ] = (uint8_t)(v      );
    d[off+1] = (uint8_t)(v >>  8);
    d[off+2] = (uint8_t)(v >> 16);
    d[off+3] = (uint8_t)(v >> 24);
}

/* ── Ring initialisation helper (write a valid ring header into slot S) ──── */

static void init_ring_slot(uint32_t slot, uint32_t pd_id) {
    volatile ld_ring_header_t *hdr = (volatile ld_ring_header_t *)
        (ring_backing + slot * RING_SIZE);
    hdr->magic = LOG_RING_MAGIC;
    hdr->pd_id = pd_id;
    hdr->head  = 0;
    hdr->tail  = 0;
}

/* ── Write bytes into a ring slot's circular buffer ──────────────────────── */

static void ring_write(uint32_t slot, const char *msg) {
    volatile ld_ring_header_t *hdr = (volatile ld_ring_header_t *)
        (ring_backing + slot * RING_SIZE);
    volatile char *buf = (volatile char *)
        (ring_backing + slot * RING_SIZE + RING_HEADER_SIZE);

    uint32_t h = hdr->head;
    for (const char *p = msg; *p; p++) {
        uint32_t next = (h + 1) % RING_BUF_SIZE;
        if (next == hdr->tail) break;  /* ring full — drop */
        buf[h] = *p;
        h = next;
    }
    hdr->head = h;
}

/* ── Test: init does not crash ───────────────────────────────────────────── */

static void test_init_ok(void) {
    setup();
    TAP_OK("log_drain_test_init completes without crash");
}

/* ── Test: OP_LOG_STATUS on fresh state ──────────────────────────────────── */

static void test_status_fresh(void) {
    setup();

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_STATUS;
    req.length = 0;

    uint32_t rc = log_drain_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc,                         (uint64_t)SEL4_ERR_OK, "OP_LOG_STATUS: returns OK");
    ASSERT_EQ(data_rd32_t(rep.data, 0),   0u, "OP_LOG_STATUS: ring_count == 0 initially");
    /* total_bytes lo and hi should both be zero */
    ASSERT_EQ(data_rd32_t(rep.data, 4),   0u, "OP_LOG_STATUS: total_bytes_lo == 0 initially");
    ASSERT_EQ(data_rd32_t(rep.data, 8),   0u, "OP_LOG_STATUS: total_bytes_hi == 0 initially");
}

/* ── Test: OP_LOG_WRITE with bad slot ────────────────────────────────────── */

static void test_write_bad_slot(void) {
    setup();

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    data_wr32_t(req.data, 0, 99u);   /* slot 99 — out of range */
    data_wr32_t(req.data, 4, 1u);    /* pd_id */
    req.length = 8;

    uint32_t rc = log_drain_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_BAD_ARG, "OP_LOG_WRITE: bad slot returns SEL4_ERR_BAD_ARG");
    ASSERT_NE(data_rd32_t(rep.data, 0), 0u, "OP_LOG_WRITE: bad slot sets error code in reply data");
}

/* ── Test: OP_LOG_WRITE registers ring on first call ─────────────────────── */

static void test_write_registers_ring(void) {
    setup();

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    data_wr32_t(req.data, 0, 0u);   /* slot 0 */
    data_wr32_t(req.data, 4, 3u);   /* pd_id = agentfs */
    req.length = 8;

    uint32_t rc = log_drain_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_LOG_WRITE: first call on unregistered ring returns OK");

    /* ring_count should now be 1 */
    sel4_msg_t sreq = {0}, srep = {0};
    sreq.opcode = OP_LOG_STATUS;
    log_drain_dispatch_one(0, &sreq, &srep);
    ASSERT_EQ(data_rd32_t(srep.data, 0), 1u, "OP_LOG_WRITE: ring_count incremented to 1 after registration");
}

/* ── Test: OP_LOG_WRITE drains bytes from ring ───────────────────────────── */

static void test_write_drains_bytes(void) {
    setup();

    /* Pre-initialise the ring slot with a valid header and some data */
    init_ring_slot(2, 5);
    ring_write(2, "hello\n");   /* 6 bytes */

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    data_wr32_t(req.data, 0, 2u);   /* slot 2 */
    data_wr32_t(req.data, 4, 5u);   /* pd_id = worker_0 */
    req.length = 8;

    uint32_t rc = log_drain_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_LOG_WRITE: drains pre-written ring data without error");

    /* total_bytes should reflect the drained data */
    sel4_msg_t sreq = {0}, srep = {0};
    sreq.opcode = OP_LOG_STATUS;
    log_drain_dispatch_one(0, &sreq, &srep);
    uint32_t total_lo = data_rd32_t(srep.data, 4);
    ASSERT_TRUE(total_lo >= 6u, "OP_LOG_WRITE: total_bytes_lo >= 6 after draining 'hello\\n'");
}

/* ── Test: OP_LOG_WRITE on already-registered ring ───────────────────────── */

static void test_write_second_call_drains(void) {
    setup();

    /* First call — registers ring */
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    data_wr32_t(req.data, 0, 1u);
    data_wr32_t(req.data, 4, 2u);   /* init_agent */
    req.length = 8;
    log_drain_dispatch_one(0, &req, &rep);

    /* Write some bytes to the ring's circular buffer directly */
    ring_write(1, "test\n");

    /* Second call — drains */
    uint32_t rc = log_drain_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_LOG_WRITE: second call on registered ring returns OK");
}

/* ── Test: OP_LOG_STATUS reflects multiple rings ─────────────────────────── */

static void test_status_multiple_rings(void) {
    setup();

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    req.length = 8;

    /* Register three different pd_ids in three slots */
    data_wr32_t(req.data, 0, 0u); data_wr32_t(req.data, 4, 1u);
    log_drain_dispatch_one(0, &req, &rep);
    data_wr32_t(req.data, 0, 1u); data_wr32_t(req.data, 4, 2u);
    log_drain_dispatch_one(0, &req, &rep);
    data_wr32_t(req.data, 0, 2u); data_wr32_t(req.data, 4, 3u);
    log_drain_dispatch_one(0, &req, &rep);

    sel4_msg_t sreq = {0}, srep = {0};
    sreq.opcode = OP_LOG_STATUS;
    log_drain_dispatch_one(0, &sreq, &srep);
    ASSERT_EQ(data_rd32_t(srep.data, 0), 3u,
              "OP_LOG_STATUS: ring_count == 3 after three registrations");
}

/* ── Test: OP_LOG_STATUS bytes accumulate ────────────────────────────────── */

static void test_status_bytes_accumulate(void) {
    setup();
    init_ring_slot(0, 0u);
    ring_write(0, "AAAA\n");   /* 5 bytes */

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    data_wr32_t(req.data, 0, 0u);
    data_wr32_t(req.data, 4, 0u);
    req.length = 8;
    log_drain_dispatch_one(0, &req, &rep);

    ring_write(0, "BBBB\n");   /* 5 more bytes */
    log_drain_dispatch_one(0, &req, &rep);

    sel4_msg_t sreq = {0}, srep = {0};
    sreq.opcode = OP_LOG_STATUS;
    log_drain_dispatch_one(0, &sreq, &srep);
    uint32_t total_lo = data_rd32_t(srep.data, 4);
    ASSERT_TRUE(total_lo >= 10u,
                "OP_LOG_STATUS: total_bytes_lo >= 10 after two drains of 5 bytes each");
}

/* ── Test: ring is drained to empty (tail catches head) ──────────────────── */

static void test_ring_drains_to_empty(void) {
    setup();
    init_ring_slot(3, 6u);
    ring_write(3, "drain_me\n");

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    data_wr32_t(req.data, 0, 3u);
    data_wr32_t(req.data, 4, 6u);
    req.length = 8;
    log_drain_dispatch_one(0, &req, &rep);

    /* After drain, head == tail in the ring header */
    volatile ld_ring_header_t *hdr = (volatile ld_ring_header_t *)
        (ring_backing + 3 * RING_SIZE);
    ASSERT_EQ(hdr->head, hdr->tail, "OP_LOG_WRITE: ring head == tail after full drain");
}

/* ── Test: OP_LOG_WRITE with rings_vaddr == 0 ────────────────────────────── */

static void test_write_no_mapping(void) {
    setup();
    log_drain_rings_vaddr = 0;   /* simulate unmapped region */
    /* re-register handlers on fresh server state */
    log_drain_test_init();
    log_drain_rings_vaddr = 0;

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    data_wr32_t(req.data, 0, 0u);
    data_wr32_t(req.data, 4, 1u);
    req.length = 8;

    uint32_t rc = log_drain_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_BAD_ARG,
              "OP_LOG_WRITE: returns error when ring region not mapped");
}

/* ── Test: unknown opcode ─────────────────────────────────────────────────── */

static void test_unknown_opcode(void) {
    setup();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xDEADu;
    req.length = 0;
    uint32_t rc = log_drain_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_INVALID_OP,
              "unknown opcode returns SEL4_ERR_INVALID_OP");
}

/* ── Test: slot 0 is valid; slot MAX_LOG_RINGS-1 is valid ────────────────── */

static void test_boundary_slots(void) {
    setup();

    /* Slot 0 */
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    data_wr32_t(req.data, 0, 0u);
    data_wr32_t(req.data, 4, 1u);
    req.length = 8;
    uint32_t rc0 = log_drain_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc0, (uint64_t)SEL4_ERR_OK, "OP_LOG_WRITE: slot 0 (lower bound) is valid");

    /* Slot MAX_LOG_RINGS-1 */
    setup();
    data_wr32_t(req.data, 0, (uint32_t)(MAX_LOG_RINGS - 1));
    data_wr32_t(req.data, 4, 2u);
    uint32_t rcN = log_drain_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rcN, (uint64_t)SEL4_ERR_OK,
              "OP_LOG_WRITE: slot MAX_LOG_RINGS-1 (upper bound) is valid");
}

/* ── Test: MAX_LOG_RINGS+1 is rejected ───────────────────────────────────── */

static void test_slot_at_max_rejected(void) {
    setup();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    data_wr32_t(req.data, 0, (uint32_t)MAX_LOG_RINGS);   /* exactly one past the end */
    data_wr32_t(req.data, 4, 1u);
    req.length = 8;
    uint32_t rc = log_drain_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_BAD_ARG,
              "OP_LOG_WRITE: slot == MAX_LOG_RINGS is rejected");
}

/* ── Test: re-registration of same pd_id uses existing state ─────────────── */

static void test_reregister_same_pd(void) {
    setup();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    data_wr32_t(req.data, 0, 0u);
    data_wr32_t(req.data, 4, 5u);
    req.length = 8;

    log_drain_dispatch_one(0, &req, &rep);
    log_drain_dispatch_one(0, &req, &rep);

    sel4_msg_t sreq = {0}, srep = {0};
    sreq.opcode = OP_LOG_STATUS;
    log_drain_dispatch_one(0, &sreq, &srep);
    ASSERT_EQ(data_rd32_t(srep.data, 0), 1u,
              "ring_count stays 1 when same pd_id registers twice");
}

/* ── Test: multiple distinct PDs each get their own ring state ───────────── */

static void test_multiple_pd_ids_distinct(void) {
    setup();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    req.length = 8;

    data_wr32_t(req.data, 0, 0u); data_wr32_t(req.data, 4, 10u);
    log_drain_dispatch_one(0, &req, &rep);
    data_wr32_t(req.data, 0, 1u); data_wr32_t(req.data, 4, 11u);
    log_drain_dispatch_one(0, &req, &rep);

    sel4_msg_t sreq = {0}, srep = {0};
    sreq.opcode = OP_LOG_STATUS;
    log_drain_dispatch_one(0, &sreq, &srep);
    ASSERT_EQ(data_rd32_t(srep.data, 0), 2u,
              "two distinct pd_ids produce ring_count == 2");
}

/* ── Test: OP_LOG_STATUS reply length is adequate ────────────────────────── */

static void test_status_reply_length(void) {
    setup();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_STATUS;
    log_drain_dispatch_one(0, &req, &rep);
    ASSERT_TRUE(rep.length >= 12u,
                "OP_LOG_STATUS: reply carries at least 12 bytes (ring_count + total_bytes)");
}

/* ── Test: badge is ignored for OP_LOG_WRITE ─────────────────────────────── */

static void test_badge_ignored(void) {
    setup();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    data_wr32_t(req.data, 0, 0u);
    data_wr32_t(req.data, 4, 1u);
    req.length = 8;

    /* Try several badge values — all should succeed */
    uint32_t rc1 = log_drain_dispatch_one(0,          &req, &rep);
    uint32_t rc2 = log_drain_dispatch_one(0xDEADu,    &req, &rep);
    uint32_t rc3 = log_drain_dispatch_one(0xFFFFFFFFu, &req, &rep);
    ASSERT_EQ(rc1, (uint64_t)SEL4_ERR_OK, "OP_LOG_WRITE: badge 0 accepted");
    ASSERT_EQ(rc2, (uint64_t)SEL4_ERR_OK, "OP_LOG_WRITE: badge 0xDEAD accepted");
    ASSERT_EQ(rc3, (uint64_t)SEL4_ERR_OK, "OP_LOG_WRITE: badge 0xFFFFFFFF accepted");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    TAP_PLAN(26);

    test_init_ok();
    test_status_fresh();
    test_write_bad_slot();
    test_write_registers_ring();
    test_write_drains_bytes();
    test_write_second_call_drains();
    test_status_multiple_rings();
    test_status_bytes_accumulate();
    test_ring_drains_to_empty();
    test_write_no_mapping();
    test_unknown_opcode();
    test_boundary_slots();
    test_slot_at_max_rejected();
    test_reregister_same_pd();
    test_multiple_pd_ids_distinct();
    test_status_reply_length();
    test_badge_ignored();   /* uses 3 ASSERT calls but counts as 1 test group */

    return tap_exit();
}

#else /* !AGENTOS_TEST_HOST */
typedef int _agentos_api_test_log_drain_dummy;
#endif /* AGENTOS_TEST_HOST */
