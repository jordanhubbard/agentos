/*
 * ipc_harness.c — agentOS IPC Test Harness Protection Domain
 *
 * Compiled into debug/test builds when CONFIG_IPC_HARNESS=1.
 * On boot, executes a static sequence of IPC health-checks against known PDs
 * and reports PASS/FAIL via the debug UART (microkit_dbg_puts).
 * Excluded from production images.
 *
 * This PD accepts no interactive input and produces no interactive output.
 * It is not a REPL. Output goes to the debug UART only.
 *
 * IPC channels:
 *   CH 0 — ipc_harness -> eventbus   (STATUS query)
 *   CH 1 — ipc_harness -> log_drain  (STATUS query)
 *   CH 2 — ipc_harness -> vm_manager (STATUS query via OP_VM_INFO)
 *
 * Build: add -DCONFIG_IPC_HARNESS to CFLAGS, or pass CONFIG_IPC_HARNESS=1 to make.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/ipc_harness_contract.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef CONFIG_IPC_HARNESS

/* ── IPC channel assignments (from harness perspective) ──────────────────── */

#define HARNESS_CH_EVENTBUS    0
#define HARNESS_CH_LOG_DRAIN   1
#define HARNESS_CH_VM_MANAGER  2

/* ── TAP output helpers ──────────────────────────────────────────────────── */

static uint32_t s_test_num   = 0;
static uint32_t s_pass_count = 0;
static uint32_t s_fail_count = 0;

static void tap_puts(const char *s) { sel4_dbg_puts(s); }

static void tap_u32(uint32_t v)
{
    char buf[12]; int i = 11; buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else { while (v && i > 0) { buf[--i] = '0' + (int)(v % 10); v /= 10; } }
    tap_puts(&buf[i]);
}

static void tap_pass(const char *name)
{
    s_test_num++;
    s_pass_count++;
    tap_puts("ok "); tap_u32(s_test_num); tap_puts(" - "); tap_puts(name); tap_puts("\n");
}

static void tap_fail(const char *name, const char *reason)
{
    s_test_num++;
    s_fail_count++;
    tap_puts("not ok "); tap_u32(s_test_num);
    tap_puts(" - "); tap_puts(name);
    tap_puts(" # "); tap_puts(reason); tap_puts("\n");
}

/* ── Test helpers ────────────────────────────────────────────────────────── */

static bool ipc_ok(uint32_t ch, uint32_t op, const char *test_name)
{
    rep_u32(rep, 0, op);
    uint32_t reply = /* E5-S8: ppcall stubbed */
    uint32_t result = (uint32_t)msg_u32(req, 0);
    bool ok = (msg_u32(req, 0) == 0) && (result == 0);
    if (ok) tap_pass(test_name);
    else    tap_fail(test_name, "non-zero result or error label");
    return ok;
}

/* ── Test suite ──────────────────────────────────────────────────────────── */

static void run_tests(void)
{
    tap_puts("TAP version 14\n");
    tap_puts("# agentOS ipc_harness — boot-time API health checks\n");

    /* EventBus: status query */
    ipc_ok(HARNESS_CH_EVENTBUS, MSG_EVENTBUS_STATUS, "eventbus.status");

    /* Log Drain: status query */
    ipc_ok(HARNESS_CH_LOG_DRAIN, OP_LOG_STATUS, "log_drain.status");

    /* VM Manager: list VMs (should return 0 at boot) */
    rep_u32(rep, 0, OP_VM_LIST);
    uint32_t vm_reply = /* E5-S8: ppcall stubbed */
    uint32_t vm_result = (uint32_t)msg_u32(req, 0);
    uint32_t vm_count  = (uint32_t)msg_u32(req, 4);
    if (vm_result == 0 && vm_count == 0) tap_pass("vm_manager.list_empty_at_boot");
    else tap_fail("vm_manager.list_empty_at_boot", "unexpected vm count or error");
    (void)vm_reply;

    /* Print summary */
    tap_puts("1.."); tap_u32(s_test_num); tap_puts("\n");
    tap_puts("# passed: "); tap_u32(s_pass_count); tap_puts("\n");
    tap_puts("# failed: "); tap_u32(s_fail_count); tap_puts("\n");
    if (s_fail_count == 0) tap_puts("# result: PASS\n");
    else                   tap_puts("# result: FAIL\n");
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

static void ipc_harness_pd_init(void)
{
    sel4_dbg_puts("[ipc_harness] starting boot-time API checks\n");
    run_tests();
    sel4_dbg_puts("[ipc_harness] done\n");
}

static void ipc_harness_pd_notified(uint32_t ch) { (void)ch; }

static uint32_t ipc_harness_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    rep_u32(rep, 0, 0xDEAD);
    rep->length = 4;
        return SEL4_ERR_OK;
}

#else /* !CONFIG_IPC_HARNESS */

/* ── Stub: excluded from production builds ───────────────────────────────── */

static void ipc_harness_pd_init(void)
{
    sel4_dbg_puts("[ipc_harness] disabled (build with CONFIG_IPC_HARNESS=1)\n");
}

static void ipc_harness_pd_notified(uint32_t ch) { (void)ch; }

static uint32_t ipc_harness_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    rep_u32(rep, 0, 0xDEAD);
    rep->length = 4;
        return SEL4_ERR_OK;
}

#endif /* CONFIG_IPC_HARNESS */

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void ipc_harness_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    ipc_harness_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, ipc_harness_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
