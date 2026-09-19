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
static unsigned teardowns;
static unsigned resets;
static unsigned console_drains;
static bool teardown_state_valid = true;
static bool suspend_fails;
static bool resume_fails;
static bool teardown_fails;
static bool reset_fails;
static uint32_t pushed_event;
static uint32_t pushed_length;
static uint8_t pushed_bytes[CC_INPUT_TEXT_MAX];

static bool start_guest(void)
{
    starts++;
    started = true;
    return true;
}

static bool suspend_guest(void) { suspends++; return !suspend_fails; }
static bool resume_guest(void) { resumes++; return !resume_fails; }
static void quiesce_timer(void) { timer_quiesces++; }
static bool teardown_guest(void)
{
    teardowns++;
    teardown_state_valid &= state == GUEST_STATE_DESTROYING;
    return !teardown_fails;
}
static bool reset_guest(void) { resets++; return !reset_fails; }
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
    console_drains++;
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
        .teardown = teardown_guest,
        .reset = reset_guest,
        .push_input = push_input,
        .drain_console = drain_console,
    };

    state = GUEST_STATE_READY;
    request(&req, MSG_GUEST_CREATE, 2u);
    failed += check(aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime) &&
                    rep.opcode == GUEST_OK && msg_u32(&rep, 4u) == 0u,
                    "matching flavor creates guest zero");

    const uint32_t unprepared[] = {
        GUEST_STATE_CREATING, GUEST_STATE_BINDING, GUEST_STATE_BOOTING,
        GUEST_STATE_RUNNING, GUEST_STATE_SUSPENDED, UINT32_MAX
    };
    for (unsigned i = 0; i < sizeof(unprepared) / sizeof(*unprepared); ++i) {
        state = unprepared[i];
        started = state == GUEST_STATE_RUNNING || state == GUEST_STATE_SUSPENDED;
        bool was_started = started;
        rep = (sel4_msg_t){0};
        failed += check(aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime) &&
                        rep.opcode == GUEST_ERR_BAD_STATE && rep.length == 0u &&
                        state == unprepared[i] && started == was_started &&
                        starts == 0u && resets == 0u,
                        "create rejects unprepared or active guest without side effects");
    }
    state = GUEST_STATE_READY;
    started = true;
    rep = (sel4_msg_t){0};
    failed += check(aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime) &&
                    rep.opcode == GUEST_ERR_BAD_STATE && rep.length == 0u &&
                    state == GUEST_STATE_READY && started && starts == 0u && resets == 0u,
                    "create rejects inconsistent ready-but-started guest");
    started = false;

    rep = (sel4_msg_t){0};
    failed += check(aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime) &&
                    rep.opcode == GUEST_OK && rep.length == 8u &&
                    msg_u32(&rep, 4u) == 0u && state == GUEST_STATE_READY &&
                    !started && starts == 0u && resets == 0u,
                    "create retry preserves an unstarted ready guest");

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
                    !started && suspends == 2u && timer_quiesces == 2u &&
                    teardowns == 1u,
                    "destroy quiesces tears down and makes state terminal");

    runtime.reset = NULL;
    request(&req, MSG_GUEST_CREATE, 2u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_DEAD &&
                    state == GUEST_STATE_DEAD && !started,
                    "destroy remains terminal without reset support");

    runtime.reset = reset_guest;
    request(&req, MSG_GUEST_CREATE, 2u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_OK && state == GUEST_STATE_READY &&
                    !started && resets == 1u,
                    "reset callback prepares dead slot for later boot");

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

    state = GUEST_STATE_RUNNING;
    started = true;
    unsigned quiesces_before = timer_quiesces;
    suspend_fails = true;
    request(&req, MSG_GUEST_SUSPEND, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_NOT_READY &&
                    state == GUEST_STATE_RUNNING &&
                    timer_quiesces == quiesces_before,
                    "failed suspend preserves running state and timer");

    request(&req, MSG_GUEST_DESTROY, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_NOT_READY &&
                    state == GUEST_STATE_RUNNING &&
                    timer_quiesces == quiesces_before,
                    "failed destroy cannot report a live guest as dead");

    runtime.suspend = NULL;
    request(&req, MSG_GUEST_SUSPEND, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_NOT_READY &&
                    state == GUEST_STATE_RUNNING,
                    "missing suspend callback fails closed");
    runtime.suspend = suspend_guest;
    suspend_fails = false;
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_OK && state == GUEST_STATE_SUSPENDED,
                    "suspend can be retried after failure");

    unsigned suspends_before = suspends;
    quiesces_before = timer_quiesces;
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_OK && suspends == suspends_before &&
                    timer_quiesces == quiesces_before,
                    "repeated suspend does not repeat execution transition");

    resume_fails = true;
    request(&req, MSG_GUEST_RESUME, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_NOT_READY &&
                    state == GUEST_STATE_SUSPENDED,
                    "failed resume preserves suspended state");
    runtime.resume = NULL;
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_NOT_READY &&
                    state == GUEST_STATE_SUSPENDED,
                    "missing resume callback fails closed");
    runtime.resume = resume_guest;
    resume_fails = false;
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_OK && state == GUEST_STATE_RUNNING,
                    "resume can be retried after failure");

    teardown_fails = true;
    suspends_before = suspends;
    quiesces_before = timer_quiesces;
    request(&req, MSG_GUEST_DESTROY, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_NOT_READY &&
                    state == GUEST_STATE_DESTROYING && started &&
                    teardowns == 2u && teardown_state_valid &&
                    suspends == suspends_before + 1u &&
                    timer_quiesces == quiesces_before + 1u,
                    "failed teardown retains the non-resumable cleanup state");

    unsigned resumes_after_teardown = resumes;
    request(&req, MSG_GUEST_RESUME, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_BAD_STATE &&
                    resumes == resumes_after_teardown,
                    "partial teardown cannot resume execution");

    const uint32_t blocked_ops[] = {
        MSG_GUEST_CREATE, MSG_GUEST_BOOT, MSG_GUEST_SUSPEND,
    };
    unsigned starts_after_teardown = starts;
    unsigned resets_after_teardown = resets;
    for (unsigned i = 0; i < sizeof(blocked_ops) / sizeof(blocked_ops[0]); i++) {
        request(&req, blocked_ops[i], 0u);
        (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
        failed += check(rep.opcode == GUEST_ERR_BAD_STATE &&
                        state == GUEST_STATE_DESTROYING &&
                        starts == starts_after_teardown &&
                        resets == resets_after_teardown &&
                        suspends == suspends_before + 1u,
                        "partial teardown rejects other lifecycle transitions");
    }

    unsigned drains_before = console_drains;
    request(&req, MSG_GUEST_CONSOLE_DRAIN, 0u);
    req.length = 8u;
    rep_u32(&req, 4u, 16u);
    (void)aos_guest_vmm_console_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_BAD_STATE &&
                    console_drains == drains_before,
                    "partial teardown cannot access console resources");

    pushed_length = 0u;
    request(&req, MSG_GUEST_SEND_INPUT, 0u);
    req.length = 29u;
    rep_u32(&req, 4u, CC_INPUT_TEXT);
    rep_u32(&req, 8u, 1u);
    req.data[28] = 'x';
    (void)aos_guest_vmm_console_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_BAD_STATE && pushed_length == 0u,
                    "partial teardown cannot deliver guest input");

    request(&req, MSG_GUEST_DESTROY, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_NOT_READY &&
                    state == GUEST_STATE_DESTROYING && teardowns == 3u &&
                    suspends == suspends_before + 1u &&
                    timer_quiesces == quiesces_before + 1u,
                    "failed cleanup retry does not touch released execution resources");

    request(&req, MSG_GUEST_DESTROY, 0u);
    teardown_fails = false;
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_OK && state == GUEST_STATE_DEAD &&
                    !started && teardowns == 4u && teardown_state_valid,
                    "teardown can be retried from cleanup state");

    reset_fails = true;
    request(&req, MSG_GUEST_CREATE, 2u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_NOT_READY &&
                    state == GUEST_STATE_DEAD && !started &&
                    resets == 2u,
                    "failed reset preserves the terminal slot");

    reset_fails = false;
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_OK && state == GUEST_STATE_READY &&
                    !started && resets == 3u,
                    "reset can be retried without entering running");

    request(&req, MSG_GUEST_BOOT, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_OK && state == GUEST_STATE_RUNNING &&
                    started,
                    "reset slot boots only after a separate boot request");

    request(&req, MSG_GUEST_SUSPEND, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    suspends_before = suspends;
    suspend_fails = true;
    request(&req, MSG_GUEST_DESTROY, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_OK && state == GUEST_STATE_DEAD &&
                    !started && suspends == suspends_before,
                    "destroy of suspended guest needs no second suspend");

    state = GUEST_STATE_SUSPENDED;
    unsigned starts_before = starts;
    unsigned resumes_before = resumes;
    request(&req, MSG_GUEST_BOOT, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_BAD_STATE &&
                    state == GUEST_STATE_SUSPENDED &&
                    starts == starts_before && resumes == resumes_before,
                    "BOOT cannot bypass a suspended execution context");

    state = GUEST_STATE_READY;
    started = false;
    request(&req, MSG_GUEST_RESUME, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_BAD_STATE &&
                    state == GUEST_STATE_READY && !started &&
                    resumes == resumes_before,
                    "RESUME cannot report an unbooted guest as running");
    suspends_before = suspends;
    request(&req, MSG_GUEST_SUSPEND, 0u);
    (void)aos_guest_vmm_lifecycle_rpc(&req, &rep, &runtime);
    failed += check(rep.opcode == GUEST_ERR_BAD_STATE &&
                    state == GUEST_STATE_READY && suspends == suspends_before,
                    "SUSPEND cannot detach an unbooted guest context");

    suspend_fails = false;
    state = GUEST_STATE_RUNNING;
    started = true;
    runtime.guest_id = 91u;
    starts_before = starts;
    unsigned resets_before = resets;
    teardown_fails = true;
    failed += check(aos_guest_vmm_restart_step(&runtime) == AOS_GUEST_RESTART_WAIT &&
                    state == GUEST_STATE_DESTROYING && starts == starts_before &&
                    resets == resets_before,
                    "restart waits for backend drain without reconstruction or entry");
    teardown_fails = false;
    failed += check(aos_guest_vmm_restart_step(&runtime) == AOS_GUEST_RESTART_RUNNING &&
                    state == GUEST_STATE_RUNNING && started &&
                    starts == starts_before + 1u && resets == resets_before + 1u &&
                    runtime.guest_id == 91u,
                    "restart reconstructs and boots with the existing nonzero identity");
    reset_fails = true;
    starts_before = starts;
    failed += check(aos_guest_vmm_restart_step(&runtime) == AOS_GUEST_RESTART_FAILED &&
                    state == GUEST_STATE_DEAD && !started && starts == starts_before,
                    "failed restart reconstruction never enters retired execution");

    printf("1..34\n");
    return failed == 0 ? 0 : 1;
}
