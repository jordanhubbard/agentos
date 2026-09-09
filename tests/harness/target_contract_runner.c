/*
 * target_contract_runner.c — on-target seL4 contract TAP runner (agentos-0h4)
 *
 * This is the Microkit protection domain that proves the *core* agentOS IPC
 * contracts against REAL seL4 IPC, on real (or QEMU-emulated) seL4 hardware.
 * It is the target-proof counterpart to the host-only mock suite that runs
 * under -DAGENTOS_TEST_HOST with the tests/microkit.h stub: where the host
 * suite issues PPCs into a stub that merely echoes MR0 back, this PD issues
 * genuine microkit_ppcall()s across real channels into the live PDs and
 * observes their real replies.
 *
 *   HOST  (mock):   make test-integration            (tests/microkit.h stub)
 *   TARGET (proof): make sel4-test-image + run-tests  (this PD, real IPC)
 *
 * Scope (the core IPC contracts named in agentos-0h4):
 *   - EventBus        (MONITOR_CH_EVENTBUS)
 *   - CC-PD           (CH_CC_PD)
 *   - serial_pd       (CH_SERIAL_PD)
 *   - log_drain       (CH_LOG_DRAIN)
 *   - guest lifecycle (CH_GUEST_PD)
 *
 * Each suite is the SAME run_*_tests(ch) function compiled into the host mock
 * suite — there is exactly one contract assertion body per PD, exercised in
 * two environments.  We do not fork a parallel set of assertions.
 *
 * Output: TAP version 14 on the serial console, terminated by the
 * "TAP_DONE:<code>" sentinel that xtask run-tests (cmd_run_tests.rs) waits on.
 * tf_tap_finish() emits the plan + pass/fail summary but NOT the sentinel, so
 * this runner emits it explicitly after the summary.
 *
 * Wiring (owned by the root-task build, see tests/TARGET_TESTS.md):
 *   Build this file plus the five tests/contracts/_test.c suites into the
 *   root task when SEL4_TEST_IMAGE=1, and call target_contract_runner_main()
 *   from main.c under #ifdef AGENTOS_SEL4_TEST_IMAGE in place of the current
 *   one-line stub TAP.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

/* ── Contract suites under test ──────────────────────────────────────────────
 *
 * test_framework.h keeps its pass/fail counters in file-`static` storage, so
 * the suites MUST share a single translation unit with this runner for the
 * TAP plan/summary to aggregate.  We therefore #include the suite .c bodies
 * directly (a unity build) rather than link them as separate objects.  Each
 * suite's "" includes resolve relative to its own directory (tests/contracts),
 * and test_framework.h / agentos.h are #pragma once / guarded.
 *
 * Scope: only PDs that actually speak seL4 IPC on their listen endpoint and
 * are present in a GUEST_OS=none test image — EventBus, serial_pd, log_drain.
 * Excluded for now (tracked in agentos-8f5 / agentos-0h4):
 *   - cc_pd: its protocol runs over virtio-serial, not a seL4 endpoint, so a
 *     microkit_ppcall() to it would block; needs a virtio-serial test driver.
 *   - guest lifecycle: no guest VMM PD exists under GUEST_OS=none; needs a
 *     guest-enabled test image.
 */
#include "../contracts/eventbus_test.c"
#include "../contracts/serial_pd_test.c"
#include "../contracts/log_drain_test.c"
#include "../../kernel/agentos-root-task/include/serial_log.h"

/* ── libmicrokit symbol shim (agentos-8f5) ───────────────────────────────────
 *
 * agentOS PDs do not link libmicrokit, and the release kernel disables
 * CONFIG_PRINTING (seL4_DebugPutChar is a no-op).  Two consequences:
 *  1. microkit.h's inline helpers reference a handful of libmicrokit externs
 *     (microkit_dbg_puts/_put32, microkit_name, microkit_pps) — we define them.
 *  2. test_framework emits via microkit_dbg_puts; route it through serial_pd's
 *     formal contract and the shared transfer page.
 *
 * microkit_pps is the bitmask of valid protected-procedure channels; if a
 * channel's bit is clear, microkit_ppcall() takes its invalid-channel branch
 * and never issues the seL4_Call.  We set all bits so every ppcall goes out.
 */
char      microkit_name[MICROKIT_PD_NAME_LENGTH] = "test_runner";
seL4_Word microkit_pps = ~(seL4_Word)0;           /* all channels valid */

#define TEST_RUNNER_SERIAL_EP ((seL4_CPtr)(74u + CH_SERIAL_PD))
static serial_log_t g_test_log = {
    .ep = TEST_RUNNER_SERIAL_EP,
};

void microkit_dbg_putc(int c) { serial_log_putc(&g_test_log, (char)c); }
void microkit_dbg_puts(const char *s) { serial_log_puts(&g_test_log, s); }
void microkit_dbg_put32(seL4_Uint32 x)
{
    char buf[11];
    int  i = 10;
    buf[i] = '\0';
    if (x == 0u) { microkit_dbg_putc('0'); return; }
    while (x > 0u && i > 0) { buf[--i] = (char)('0' + (x % 10u)); x /= 10u; }
    microkit_dbg_puts(&buf[i]);
}

/* ── Emit the run-tests sentinel ──────────────────────────────────────────── */
/*
 * tf_tap_finish() prints "1..N", "# passed", "# failed" but intentionally does
 * NOT print the TAP_DONE sentinel (it is shared with the simulator harness,
 * which has its own completion path).  cmd_run_tests.rs::parse_tap_done() keys
 * off "TAP_DONE:<code>" with code 0 == pass, so derive the code from the
 * framework's running fail counter.
 */
static inline void target_tap_done(void)
{
    _tf_puts("TAP_DONE:");
    _tf_put_uint((uint64_t)(_tf_fail > 0 ? 1 : 0));
    _tf_puts("\n");
}

/* ── Entry point ──────────────────────────────────────────────────────────── */
/*
 * Run the core contract suites against real channels, then emit the summary
 * and the sentinel.  Channels come from agentos.h and match the controller's
 * view of each PD endpoint.
 */
void target_contract_runner_main(void)
{
    tf_tap_init("agentOS-target-contracts");

    run_eventbus_tests((microkit_channel)MONITOR_CH_EVENTBUS);
    run_serial_pd_tests((microkit_channel)CH_SERIAL_PD);
    run_log_drain_tests((microkit_channel)CH_LOG_DRAIN);
    _tf_puts("# skip: cc_pd (virtio-serial protocol) + guest (no VMM under GUEST_OS=none)\n");

    tf_tap_finish();
    target_tap_done();
}

/* ── PD entry point ──────────────────────────────────────────────────────────
 *
 * pd_entry.c calls pd_main(my_ep, ns_ep).  The runner is a pure client: it
 * issues PPCs to the service PDs and needs neither its own server endpoint nor
 * the nameserver.  After emitting TAP_DONE (which the run-tests harness waits
 * on, then tears down QEMU) it parks.
 */
void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)my_ep;
    (void)ns_ep;
    target_contract_runner_main();
    for (;;) {
        seL4_Yield();
    }
}
