/*
 * agentOS cc_contract — Contract Unit Tests
 *
 * Validates all MSG_CC_* opcodes, struct layouts, error codes, device type
 * constants, and input event types without seL4 or Microkit.  No cc_pd
 * implementation is tested — the tests verify only that the API definitions
 * in cc_contract.h are internally consistent and compile cleanly on a host.
 *
 * Build:  cc -o /tmp/test_cc_contract \
 *             tests/test_cc_contract.c \
 *             -I tests \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST
 * Run:    /tmp/test_cc_contract
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "contracts/cc_contract.h"

/* ══════════════════════════════════════════════════════════════════════════
 * Test infrastructure
 * ══════════════════════════════════════════════════════════════════════════ */

#define PASS(name)  do { printf("  PASS  %s\n", name); return 0; } while(0)
#define FAIL(msg)   do { printf("  FAIL  %s:%d: %s\n", __FILE__, __LINE__, msg); return 1; } while(0)
#define CHECK(cond) do { if (!(cond)) FAIL(#cond); } while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * Opcode tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_cc_session_opcodes(void)
{
    CHECK((MSG_CC_CONNECT    & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_DISCONNECT & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_SEND       & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_RECV       & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_STATUS     & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_LIST       & 0xFF00u) == 0x2600u);

    CHECK(MSG_CC_CONNECT    == 0x2601u);
    CHECK(MSG_CC_DISCONNECT == 0x2602u);
    CHECK(MSG_CC_SEND       == 0x2603u);
    CHECK(MSG_CC_RECV       == 0x2604u);
    CHECK(MSG_CC_STATUS     == 0x2605u);
    CHECK(MSG_CC_LIST       == 0x2606u);

    PASS("cc_session_opcodes");
}

static int test_cc_relay_opcodes(void)
{
    CHECK(MSG_CC_LIST_GUESTS        == 0x2607u);
    CHECK(MSG_CC_LIST_DEVICES       == 0x2608u);
    CHECK(MSG_CC_LIST_POLECATS      == 0x2609u);
    CHECK(MSG_CC_GUEST_STATUS       == 0x260Au);
    CHECK(MSG_CC_DEVICE_STATUS      == 0x260Bu);
    CHECK(MSG_CC_ATTACH_FRAMEBUFFER == 0x260Cu);
    CHECK(MSG_CC_SEND_INPUT         == 0x260Du);
    CHECK(MSG_CC_SNAPSHOT           == 0x260Eu);
    CHECK(MSG_CC_RESTORE            == 0x260Fu);
    CHECK(MSG_CC_LOG_STREAM         == 0x2610u);

    /* All in 0x2600 range */
    CHECK((MSG_CC_LIST_GUESTS        & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_LIST_DEVICES       & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_LIST_POLECATS      & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_GUEST_STATUS       & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_DEVICE_STATUS      & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_ATTACH_FRAMEBUFFER & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_SEND_INPUT         & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_SNAPSHOT           & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_RESTORE            & 0xFF00u) == 0x2600u);
    CHECK((MSG_CC_LOG_STREAM         & 0xFF00u) == 0x2600u);

    /* All opcodes must be unique */
    uint32_t ops[] = {
        MSG_CC_CONNECT, MSG_CC_DISCONNECT, MSG_CC_SEND, MSG_CC_RECV,
        MSG_CC_STATUS, MSG_CC_LIST, MSG_CC_LIST_GUESTS, MSG_CC_LIST_DEVICES,
        MSG_CC_LIST_POLECATS, MSG_CC_GUEST_STATUS, MSG_CC_DEVICE_STATUS,
        MSG_CC_ATTACH_FRAMEBUFFER, MSG_CC_SEND_INPUT, MSG_CC_SNAPSHOT,
        MSG_CC_RESTORE, MSG_CC_LOG_STREAM,
    };
    size_t n = sizeof(ops) / sizeof(ops[0]);
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            CHECK(ops[i] != ops[j]);

    PASS("cc_relay_opcodes");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Error code tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_cc_error_codes(void)
{
    CHECK(CC_OK                == 0);
    CHECK(CC_ERR_NO_SESSIONS   == 1);
    CHECK(CC_ERR_BAD_SESSION   == 2);
    CHECK(CC_ERR_EXPIRED       == 3);
    CHECK(CC_ERR_CMD_TOO_LARGE == 4);
    CHECK(CC_ERR_NO_RESPONSE   == 5);
    CHECK(CC_ERR_BAD_HANDLE    == 6);
    CHECK(CC_ERR_BAD_DEV_TYPE  == 7);
    CHECK(CC_ERR_RELAY_FAULT   == 8);

    PASS("cc_error_codes");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Device type constant tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_cc_dev_type_constants(void)
{
    CHECK(CC_DEV_TYPE_SERIAL == 0u);
    CHECK(CC_DEV_TYPE_NET    == 1u);
    CHECK(CC_DEV_TYPE_BLOCK  == 2u);
    CHECK(CC_DEV_TYPE_USB    == 3u);
    CHECK(CC_DEV_TYPE_FB     == 4u);
    CHECK(CC_DEV_TYPE_COUNT  == 5u);

    PASS("cc_dev_type_constants");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Input event type tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_cc_input_event_types(void)
{
    CHECK(CC_INPUT_KEY_DOWN   == 0x01u);
    CHECK(CC_INPUT_KEY_UP     == 0x02u);
    CHECK(CC_INPUT_MOUSE_MOVE == 0x03u);
    CHECK(CC_INPUT_MOUSE_BTN  == 0x04u);

    /* All distinct */
    CHECK(CC_INPUT_KEY_DOWN   != CC_INPUT_KEY_UP);
    CHECK(CC_INPUT_KEY_DOWN   != CC_INPUT_MOUSE_MOVE);
    CHECK(CC_INPUT_KEY_DOWN   != CC_INPUT_MOUSE_BTN);
    CHECK(CC_INPUT_KEY_UP     != CC_INPUT_MOUSE_MOVE);
    CHECK(CC_INPUT_KEY_UP     != CC_INPUT_MOUSE_BTN);
    CHECK(CC_INPUT_MOUSE_MOVE != CC_INPUT_MOUSE_BTN);

    PASS("cc_input_event_types");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Struct layout tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_cc_guest_info_layout(void)
{
    cc_guest_info_t g;
    CHECK(sizeof(g) == 4 * sizeof(uint32_t));
    CHECK(sizeof(g.guest_handle) == sizeof(uint32_t));
    CHECK(sizeof(g.state)        == sizeof(uint32_t));
    CHECK(sizeof(g.os_type)      == sizeof(uint32_t));
    CHECK(sizeof(g.arch)         == sizeof(uint32_t));

    memset(&g, 0, sizeof(g));
    g.guest_handle = 0xDEADBEEFu;
    g.state        = 0xCAFEu;
    g.os_type      = 0x01u;
    g.arch         = 0x02u;
    CHECK(g.guest_handle == 0xDEADBEEFu);
    CHECK(g.state        == 0xCAFEu);
    CHECK(g.os_type      == 0x01u);
    CHECK(g.arch         == 0x02u);

    PASS("cc_guest_info_layout");
}

static int test_cc_guest_status_layout(void)
{
    cc_guest_status_t s;
    CHECK(sizeof(s) == 8 * sizeof(uint32_t));

    memset(&s, 0xFF, sizeof(s));
    s.guest_handle = 1u;
    s.state        = 2u;
    s.os_type      = 3u;
    s.arch         = 0u;
    s.device_flags = 0x1Fu;
    CHECK(s.guest_handle == 1u);
    CHECK(s.state        == 2u);
    CHECK(s.os_type      == 3u);
    CHECK(s.arch         == 0u);
    CHECK(s.device_flags == 0x1Fu);

    PASS("cc_guest_status_layout");
}

static int test_cc_device_info_layout(void)
{
    cc_device_info_t d;
    CHECK(sizeof(d) == 4 * sizeof(uint32_t));

    memset(&d, 0, sizeof(d));
    d.dev_type   = CC_DEV_TYPE_NET;
    d.dev_handle = 42u;
    d.state      = 1u;
    CHECK(d.dev_type   == CC_DEV_TYPE_NET);
    CHECK(d.dev_handle == 42u);
    CHECK(d.state      == 1u);

    PASS("cc_device_info_layout");
}

static int test_cc_input_event_layout(void)
{
    cc_input_event_t ev;
    CHECK(sizeof(ev) == 6 * sizeof(uint32_t));

    memset(&ev, 0, sizeof(ev));
    ev.event_type = CC_INPUT_KEY_DOWN;
    ev.keycode    = 0x70u;  /* HID F1 */
    ev.dx         = 0;
    ev.dy         = 0;
    ev.btn_mask   = 0u;
    CHECK(ev.event_type == CC_INPUT_KEY_DOWN);
    CHECK(ev.keycode    == 0x70u);

    /* Test mouse move fields */
    ev.event_type = CC_INPUT_MOUSE_MOVE;
    ev.dx         = -5;
    ev.dy         = 3;
    CHECK(ev.event_type == CC_INPUT_MOUSE_MOVE);
    CHECK(ev.dx         == -5);
    CHECK(ev.dy         == 3);

    PASS("cc_input_event_layout");
}

static int test_cc_session_info_layout(void)
{
    cc_session_info_t si;
    CHECK(sizeof(si) == 4 * sizeof(uint32_t));

    memset(&si, 0, sizeof(si));
    si.session_id         = 3u;
    si.state              = CC_SESSION_STATE_BUSY;
    si.client_badge       = 0xABCDu;
    si.ticks_since_active = 100u;
    CHECK(si.session_id         == 3u);
    CHECK(si.state              == (uint32_t)CC_SESSION_STATE_BUSY);
    CHECK(si.client_badge       == 0xABCDu);
    CHECK(si.ticks_since_active == 100u);

    PASS("cc_session_info_layout");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Configuration constant tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_cc_config_constants(void)
{
    CHECK(CC_MAX_SESSIONS > 0u);
    CHECK(CC_MAX_SESSIONS <= 64u);
    CHECK(CC_SESSION_TIMEOUT_TICKS > 0u);
    CHECK(CC_MAX_CMD_BYTES > 0u);
    CHECK(CC_MAX_RESP_BYTES > 0u);

    PASS("cc_config_constants");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Session state constant tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_cc_session_states(void)
{
    CHECK(CC_SESSION_STATE_CONNECTED != CC_SESSION_STATE_IDLE);
    CHECK(CC_SESSION_STATE_CONNECTED != CC_SESSION_STATE_BUSY);
    CHECK(CC_SESSION_STATE_CONNECTED != CC_SESSION_STATE_EXPIRED);
    CHECK(CC_SESSION_STATE_IDLE      != CC_SESSION_STATE_BUSY);
    CHECK(CC_SESSION_STATE_IDLE      != CC_SESSION_STATE_EXPIRED);
    CHECK(CC_SESSION_STATE_BUSY      != CC_SESSION_STATE_EXPIRED);

    PASS("cc_session_states");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Request/reply struct size tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_cc_req_reply_sizes(void)
{
    /* Session management */
    CHECK(sizeof(struct cc_req_connect)    == 2 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_disconnect) == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_send)       == 3 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_recv)       == 2 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_status)     == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_list)       == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_connect)  == 2 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_send)     == 2 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_recv)     == 2 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_status)   == 4 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_list)     == 1 * sizeof(uint32_t));

    /* Relay API */
    CHECK(sizeof(struct cc_req_list_guests)       == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_list_guests)     == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_list_devices)      == 2 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_list_devices)    == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_list_polecats)   == 4 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_guest_status)      == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_guest_status)    == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_device_status)     == 2 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_device_status)   == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_attach_framebuffer)  == 2 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_attach_framebuffer) == 2 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_send_input)        == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_send_input)      == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_snapshot)          == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_snapshot)        == 3 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_restore)           == 3 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_restore)         == 1 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_req_log_stream)        == 2 * sizeof(uint32_t));
    CHECK(sizeof(struct cc_reply_log_stream)      == 2 * sizeof(uint32_t));

    PASS("cc_req_reply_sizes");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MSG_GUEST_SEND_INPUT opcode test
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_msg_guest_send_input(void)
{
    CHECK(MSG_GUEST_SEND_INPUT == 0x2A08u);
    CHECK((MSG_GUEST_SEND_INPUT & 0xFF00u) == 0x2A00u);
    /* Must be distinct from all other MSG_GUEST_* */
    CHECK(MSG_GUEST_SEND_INPUT != MSG_GUEST_CREATE);
    CHECK(MSG_GUEST_SEND_INPUT != MSG_GUEST_BIND_DEVICE);
    CHECK(MSG_GUEST_SEND_INPUT != MSG_GUEST_SET_MEMORY);
    CHECK(MSG_GUEST_SEND_INPUT != MSG_GUEST_BOOT);
    CHECK(MSG_GUEST_SEND_INPUT != MSG_GUEST_SUSPEND);
    CHECK(MSG_GUEST_SEND_INPUT != MSG_GUEST_RESUME);
    CHECK(MSG_GUEST_SEND_INPUT != MSG_GUEST_DESTROY);

    PASS("msg_guest_send_input");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

typedef int (*test_fn)(void);

static const test_fn tests[] = {
    test_cc_session_opcodes,
    test_cc_relay_opcodes,
    test_cc_error_codes,
    test_cc_dev_type_constants,
    test_cc_input_event_types,
    test_cc_guest_info_layout,
    test_cc_guest_status_layout,
    test_cc_device_info_layout,
    test_cc_input_event_layout,
    test_cc_session_info_layout,
    test_cc_config_constants,
    test_cc_session_states,
    test_cc_req_reply_sizes,
    test_msg_guest_send_input,
};

int main(void)
{
    printf("cc_contract tests\n");
    int failed = 0;
    size_t n = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < n; i++)
        failed += tests[i]();
    printf("%s (%zu/%zu passed)\n",
           failed == 0 ? "ALL PASS" : "FAILURES DETECTED",
           n - (size_t)failed, n);
    return failed == 0 ? 0 : 1;
}
