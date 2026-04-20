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

static void tap_puts(const char *s) { microkit_dbg_puts(s); }

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

static bool ipc_ok(microkit_channel ch, uint32_t op, const char *test_name)
{
    microkit_mr_set(0, op);
    microkit_msginfo reply = microkit_ppcall(ch, microkit_msginfo_new(op, 1));
    uint32_t result = (uint32_t)microkit_mr_get(0);
    bool ok = (microkit_msginfo_get_label(reply) == 0) && (result == 0);
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
    microkit_mr_set(0, OP_VM_LIST);
    microkit_msginfo vm_reply = microkit_ppcall(HARNESS_CH_VM_MANAGER,
                                                microkit_msginfo_new(OP_VM_LIST, 1));
    uint32_t vm_result = (uint32_t)microkit_mr_get(0);
    uint32_t vm_count  = (uint32_t)microkit_mr_get(1);
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

void init(void)
{
    microkit_dbg_puts("[ipc_harness] starting boot-time API checks\n");
    run_tests();
    microkit_dbg_puts("[ipc_harness] done\n");
}

void notified(microkit_channel ch) { (void)ch; }

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch; (void)msginfo;
    microkit_mr_set(0, 0xDEAD);
    return microkit_msginfo_new(0, 1);
}

#else /* !CONFIG_IPC_HARNESS */

/* ── Stub: excluded from production builds ───────────────────────────────── */

void init(void)
{
    microkit_dbg_puts("[ipc_harness] disabled (build with CONFIG_IPC_HARNESS=1)\n");
}

void notified(microkit_channel ch) { (void)ch; }

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch; (void)msginfo;
    microkit_mr_set(0, 0xDEAD);
    return microkit_msginfo_new(0, 1);
}

#endif /* CONFIG_IPC_HARNESS */
