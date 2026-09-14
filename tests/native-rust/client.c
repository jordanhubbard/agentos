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
    serial_log_puts(&log_channel,
        "[native-rust] PASS: IPC version, all 120 MRs, invalid requests, recovery\n");
    for (;;) seL4_Yield();
}
