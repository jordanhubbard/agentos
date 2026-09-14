#include "agentos.h"
#include "serial_log.h"
#include "system_desc.h"
#include "contracts/native_rust_probe.h"

uintptr_t log_drain_rings_vaddr;
static serial_log_t log_channel = {.ep = PD_CNODE_SLOT_SERIAL_EP};

static void check(int condition, const char *failure)
{
    if (condition) return;
    serial_log_puts(&log_channel, "[native-rust] FAIL: ");
    serial_log_puts(&log_channel, failure);
    serial_log_puts(&log_channel, "\n");
    for (;;) seL4_Yield();
}

static seL4_MessageInfo_t invoke(seL4_Word label, seL4_Word count)
{
    return seL4_Call(NATIVE_RUST_PROBE_ENDPOINT,
                    seL4_MessageInfo_new(label, 0, 0, count));
}

static void expect_error(seL4_Word label, seL4_Word count, seL4_Word expected)
{
    seL4_MessageInfo_t reply = invoke(label, count);
    check(seL4_MessageInfo_get_label(reply) == expected &&
          seL4_MessageInfo_get_length(reply) == 0, "wrong error response");
}

void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)endpoint;
    (void)nameserver;
    seL4_MessageInfo_t badge_reply = invoke(NATIVE_RUST_BADGE, 0);
    check(seL4_MessageInfo_get_label(badge_reply) == NATIVE_RUST_OK &&
          seL4_MessageInfo_get_length(badge_reply) == 1, "badge response");
    seL4_Word badge = seL4_GetMR(0);
    check((badge >> 48) == SVC_ID_NATIVE_RUST_PROBE &&
          ((badge >> 32) & 0xffff) != 0 && (uint32_t)badge == 0,
          "root caller badge not delivered");

    for (unsigned round = 0; round < 3; round++) {
        seL4_SetMR(0, NATIVE_RUST_VERSION);
        for (unsigned i = 1; i < NATIVE_RUST_WORDS; i++)
            seL4_SetMR(i, ((seL4_Word)round << 48) | ((seL4_Word)i << 16) | i);
        seL4_MessageInfo_t reply = invoke(NATIVE_RUST_ECHO, NATIVE_RUST_WORDS);
        check(seL4_MessageInfo_get_label(reply) == NATIVE_RUST_OK &&
              seL4_MessageInfo_get_length(reply) == NATIVE_RUST_WORDS,
              "full-buffer reply shape");
        check(seL4_GetMR(0) == NATIVE_RUST_VERSION, "version response");
        for (unsigned i = 1; i < NATIVE_RUST_WORDS; i++) {
            seL4_Word sent = ((seL4_Word)round << 48) | ((seL4_Word)i << 16) | i;
            check(seL4_GetMR(i) == (sent ^ (NATIVE_RUST_SALT + i)),
                  "message word mismatch");
        }
        expect_error(0xffff, 0, NATIVE_RUST_ERR_OPCODE);
        expect_error(NATIVE_RUST_ECHO, 0, NATIVE_RUST_ERR_LENGTH);
        expect_error(NATIVE_RUST_ECHO, NATIVE_RUST_WORDS - 1, NATIVE_RUST_ERR_LENGTH);
        seL4_SetMR(0, NATIVE_RUST_VERSION + 1);
        expect_error(NATIVE_RUST_ECHO, NATIVE_RUST_WORDS, NATIVE_RUST_ERR_VERSION);
    }
    for (unsigned seed = 17; seed <= 93; seed += 76) {
        expect_error(NATIVE_RUST_EXECUTOR, 0, NATIVE_RUST_ERR_LENGTH);
        seL4_SetMR(0, NATIVE_RUST_VERSION + 1);
        expect_error(NATIVE_RUST_EXECUTOR, 1, NATIVE_RUST_ERR_VERSION);
        seL4_SetMR(0, NATIVE_RUST_VERSION);
        seL4_MessageInfo_t execution = invoke(NATIVE_RUST_EXECUTOR, 1);
        check(seL4_MessageInfo_get_label(execution) == NATIVE_RUST_OK &&
              seL4_MessageInfo_get_length(execution) == 8, "executor response");
        const uint64_t expected[] = {NATIVE_RUST_VERSION, 6, 2, 1, 0, 1, 3, 0};
        for (unsigned i = 0; i < 8; i++)
            check(seL4_GetMR(i) == expected[i], "executor budgets, capacity or cancellation");
        /* Full-heap reuse below also checks that executor-owned allocations
         * were released after its async tasks completed or were cancelled. */
        expect_error(NATIVE_RUST_HEAP, 0, NATIVE_RUST_ERR_LENGTH);
        seL4_SetMR(0, NATIVE_RUST_VERSION + 1);
        seL4_SetMR(1, seed);
        expect_error(NATIVE_RUST_HEAP, 2, NATIVE_RUST_ERR_VERSION);
        seL4_SetMR(0, NATIVE_RUST_VERSION);
        seL4_SetMR(1, 256);
        expect_error(NATIVE_RUST_HEAP, 2, NATIVE_RUST_ERR_HEAP);
        seL4_SetMR(0, NATIVE_RUST_VERSION);
        seL4_SetMR(1, seed);
        seL4_MessageInfo_t reply = invoke(NATIVE_RUST_HEAP, 2);
        check(seL4_MessageInfo_get_label(reply) == NATIVE_RUST_OK &&
              seL4_MessageInfo_get_length(reply) == 6, "heap proof response");
        uint64_t checksum = 0;
        for (unsigned i = 0; i < 2049; i++)
            checksum = ((checksum << 5) | (checksum >> 59)) ^ ((i * 37 + seed) & 255u);
        check(seL4_GetMR(0) == NATIVE_RUST_VERSION && seL4_GetMR(1) == 2049 &&
              seL4_GetMR(2) == checksum && seL4_GetMR(3) == 0 &&
              seL4_GetMR(4) == 1 && seL4_GetMR(5) == 1,
              "heap payload, alignment, exhaustion or reuse");
    }
    expect_error(NATIVE_RUST_NETWORK, 0, NATIVE_RUST_ERR_LENGTH);
    seL4_SetMR(0, NATIVE_RUST_VERSION);
    seL4_MessageInfo_t network = invoke(NATIVE_RUST_NETWORK, 1);
    check(seL4_MessageInfo_get_label(network) == NATIVE_RUST_OK &&
          seL4_MessageInfo_get_length(network) == 3 &&
          seL4_GetMR(0) == 1 && seL4_GetMR(1) == 3 && seL4_GetMR(2) >= 3,
          "native queue NIC traffic and persistent wakeups");
    serial_log_puts(&log_channel,
        "[native-rust] PASS: isolated network queues, NIC ARP replies, persistent wakeups\n");
    serial_log_puts(&log_channel,
        "[native-rust] PASS: async tasks, poll budgets, capacity, cancellation\n");
    serial_log_puts(&log_channel,
        "[native-rust] PASS: alloc Vec, alignment, exhaustion, heap reuse\n");
    serial_log_puts(&log_channel,
        "[native-rust] PASS: IPC version, all 120 MRs, invalid requests, recovery\n");
    for (;;) seL4_Yield();
}
