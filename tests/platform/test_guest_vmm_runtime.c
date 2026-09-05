#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "agentos.h"
#include "contracts/cc_contract.h"
#include "contracts/guest_contract.h"
#include <platform/guest_vmm_runtime.h>

static bool started;
static uint32_t state;
static unsigned starts;
static unsigned suspends;
static unsigned resumes;
static unsigned timer_quiesces;
static uint32_t pushed_event;
static uint32_t pushed_length;
static uint8_t pushed_bytes[CC_INPUT_TEXT_MAX];

static bool start_guest(void)
{
    starts++;
    started = true;
    return true;
}

static void suspend_guest(void) { suspends++; }
static void resume_guest(void) { resumes++; }
static void quiesce_timer(void) { timer_quiesces++; }
static bool push_input(uint32_t event_type, const uint8_t *bytes,
                       uint32_t length)
{
    pushed_event = event_type;
    pushed_length = length;
    for (uint32_t i = 0u; i < length; i++) pushed_bytes[i] = bytes[i];
    return true;
}
static uint32_t drain_console(uint8_t *bytes, uint32_t capacity)
{
    static const uint8_t output[] = {'t', 't', 'y'};
    uint32_t length = capacity < sizeof(output) ? capacity : sizeof(output);
    for (uint32_t i = 0u; i < length; i++) bytes[i] = output[i];
    return length;
}

static void request(sel4_msg_t *req, uint32_t opcode, uint32_t value)
{
    *req = (sel4_msg_t){0};
    req->opcode = opcode;
    req->length = 4u;
    rep_u32(req, 0u, value);
}

static int check(bool condition, const char *name)
{
    printf("%s - %s\n", condition ? "ok" : "not ok", name);
    return condition ? 0 : 1;
}

int main(void)
{
    int failed = 0;
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    aos_guest_vmm_runtime_t runtime = {
        .os_type = 2u,
        .guest_id = 0u,
        .state = &state,
        .started = &started,
        .start = start_guest,
        .suspend = suspend_guest,
        .resume = resume_guest,
        .quiesce_timer = quiesce_timer,
        .push_input = push_input,
        .drain_console = drain_console,
    };

    state = GUEST_STATE_READY;
    request(&req, MSG_GUEST_CREATE, 2u);
    failed += check(aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime) &&
                    rep.opcode == GUEST_OK && msg_u32(&rep, 4u) == 0u,
                    "matching flavor creates guest zero");

    rep = (sel4_msg_t){0};
    request(&req, MSG_GUEST_BOOT, 0u);
    failed += check(aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime) &&
                    rep.opcode == GUEST_OK && state == GUEST_STATE_RUNNING &&
                    started && starts == 1u,
                    "boot starts configured flavor");

    rep = (sel4_msg_t){0};
    request(&req, MSG_GUEST_SEND_INPUT, 0u);
    req.length = 31u;
    rep_u32(&req, 4u, CC_INPUT_TEXT);
    rep_u32(&req, 8u, 3u);
    req.data[28] = 'a';
    req.data[29] = 'b';
    req.data[30] = 'c';
    failed += check(aos_guest_vmm_console_rpc(&req, &rep, &runtime) &&
                    rep.opcode == GUEST_OK &&
                    pushed_event == CC_INPUT_TEXT && pushed_length == 3u &&
                    pushed_bytes[0] == 'a' && pushed_bytes[2] == 'c',
                    "text input uses shared validation and flavor callback");

    rep = (sel4_msg_t){0};
    request(&req, MSG_GUEST_SEND_INPUT, 0u);
    req.length = 28u;
    rep_u32(&req, 4u, CC_INPUT_KEY_DOWN);
    rep_u32(&req, 8u, 0x04u);
    failed += check(aos_guest_vmm_console_rpc(&req, &rep, &runtime) &&
                    rep.opcode == GUEST_OK && pushed_length == 1u &&
                    pushed_bytes[0] == 'a',
                    "key input is decoded before flavor delivery");

    rep = (sel4_msg_t){0};
    request(&req, MSG_GUEST_CONSOLE_DRAIN, 0u);
    req.length = 8u;
    rep_u32(&req, 4u, 2u);
    failed += check(aos_guest_vmm_console_rpc(&req, &rep, &runtime) &&
                    rep.opcode == GUEST_OK && rep.length == 2u &&
                    rep.data[0] == 't' && rep.data[1] == 't',
                    "console drain is bounded by caller capacity");

    rep = (sel4_msg_t){0};
    request(&req, MSG_GUEST_SEND_INPUT, 0u);
    req.length = 28u;
    rep_u32(&req, 4u, CC_INPUT_TEXT);
    rep_u32(&req, 8u, CC_INPUT_TEXT_MAX + 1u);
    failed += check(aos_guest_vmm_console_rpc(&req, &rep, &runtime) &&
                    rep.opcode == GUEST_ERR_PROTOCOL_VIOLATION,
                    "oversized text is rejected centrally");

    rep = (sel4_msg_t){0};
    request(&req, MSG_GUEST_SUSPEND, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_OK &&
                    state == GUEST_STATE_SUSPENDED &&
                    suspends == 1u && timer_quiesces == 1u,
                    "suspend quiesces guest and timer");

    rep = (sel4_msg_t){0};
    request(&req, MSG_GUEST_RESUME, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_OK &&
                    state == GUEST_STATE_RUNNING && resumes == 1u,
                    "resume returns guest to running");

    rep = (sel4_msg_t){0};
    request(&req, MSG_GUEST_DESTROY, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_OK && state == GUEST_STATE_DEAD &&
                    suspends == 2u && timer_quiesces == 2u,
                    "destroy quiesces and makes state terminal");

    rep = (sel4_msg_t){0};
    request(&req, MSG_GUEST_BOOT, 1u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_BAD_GUEST_ID,
                    "wrong guest id is rejected");

    rep = (sel4_msg_t){0};
    request(&req, MSG_GUEST_CREATE, 1u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_BAD_OS_TYPE,
                    "wrong flavor is rejected");

    {
        uint8_t byte = 0u;
        failed += check(aos_guest_vmm_input_event_to_byte(
                            CC_INPUT_KEY_DOWN, 0x04u, &byte) && byte == 'a' &&
                        aos_guest_vmm_input_event_to_byte(
                            CC_INPUT_KEY_DOWN, 0x10du, &byte) && byte == '\r',
                        "shared HID and raw-byte decoding is stable");
    }

    printf("1..12\n");
    return failed == 0 ? 0 : 1;
}
