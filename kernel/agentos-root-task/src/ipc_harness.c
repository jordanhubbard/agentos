/*
 * ipc_harness.c — agentOS IPC Test Harness Protection Domain
 *
 * Programmatic IPC test PD. Conditionally compiled when CONFIG_IPC_HARNESS=1.
 * On init(), executes a static sequence of IPC calls against known PDs and
 * reports results via the log drain. The PD then goes idle and relies on
 * Microkit notified()/protected() to stay alive.
 *
 * IPC channels (from ipc_harness perspective):
 *   IPC_HARNESS_CH_EVENTBUS  (0) — PPCs into event_bus: MSG_EVENTBUS_STATUS
 *   IPC_HARNESS_CH_INITAGENT (1) — PPCs into init_agent: MSG_INITAGENT_STATUS
 *   IPC_HARNESS_CH_WATCHDOG  (2) — PPCs into watchdog_pd: OP_WD_STATUS
 *                                  (enabled only when IPC_HARNESS_HAS_WATCHDOG is set)
 *   CH_LOG_DRAIN           (60) — log drain output (log_drain_write)
 *
 * Build:
 *   Default: no-op stub — always built so the system image stays valid.
 *   Full harness: pass -DCONFIG_IPC_HARNESS=1 (set CONFIG_IPC_HARNESS=1 in make).
 *   Watchdog test: additionally pass -DIPC_HARNESS_HAS_WATCHDOG when watchdog_pd
 *                  is present in the system description.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>
#include <stddef.h>

/*
 * log_drain_rings_vaddr — seL4cp setvar bound to log_drain_rings mapping.
 * Declared extern in agentos.h (used by log_drain_write()); defined here so the
 * Microkit linker can set it via setvar_vaddr="log_drain_rings_vaddr".
 */
uintptr_t log_drain_rings_vaddr;

#ifdef CONFIG_IPC_HARNESS

/* ── Logging ──────────────────────────────────────────────────────────────── */

static void ipc_log(const char *msg)
{
    log_drain_write(IPC_HARNESS_LOG_SLOT, IPC_HARNESS_PD_ID, msg);
}

/* Write a single digit character — used for the N/M summary line. */
static void ipc_log_digit(int n)
{
    char s[2] = { '0' + (char)(n % 10), '\0' };
    ipc_log(s);
}

/* ── Individual IPC tests ─────────────────────────────────────────────────── */

/*
 * Test 1: Send MSG_EVENTBUS_STATUS to event_bus.
 * A functioning event_bus sets MR0 to a non-error value on reply.
 */
static int test_eventbus_status(void)
{
    microkit_ppcall(IPC_HARNESS_CH_EVENTBUS,
                    microkit_msginfo_new(MSG_EVENTBUS_STATUS, 0));
    uint32_t rc = (uint32_t)microkit_mr_get(0);
    int pass = (rc != 0xDEAD);
    ipc_log(pass ? "[ipc_harness] PASS: eventbus STATUS\n"
                 : "[ipc_harness] FAIL: eventbus STATUS (no handler)\n");
    return pass;
}

/*
 * Test 2: Send MSG_INITAGENT_STATUS to init_agent.
 * A functioning init_agent sets MR0 to a non-error value on reply.
 */
static int test_initagent_status(void)
{
    microkit_ppcall(IPC_HARNESS_CH_INITAGENT,
                    microkit_msginfo_new(MSG_INITAGENT_STATUS, 0));
    uint32_t rc = (uint32_t)microkit_mr_get(0);
    int pass = (rc != 0xDEAD);
    ipc_log(pass ? "[ipc_harness] PASS: init_agent STATUS\n"
                 : "[ipc_harness] FAIL: init_agent STATUS (no handler)\n");
    return pass;
}

#ifdef IPC_HARNESS_HAS_WATCHDOG
/*
 * Test 3: Send OP_WD_STATUS to watchdog_pd with slot_id=0.
 * Expects WD_OK (0x00) or WD_ERR_NOENT (0x01) in MR0.
 * Requires IPC_HARNESS_CH_WATCHDOG wired in the .system file.
 */
static int test_watchdog_status(void)
{
    microkit_mr_set(0, OP_WD_STATUS);
    microkit_mr_set(1, 0);   /* slot_id = 0 */
    microkit_ppcall(IPC_HARNESS_CH_WATCHDOG,
                    microkit_msginfo_new(OP_WD_STATUS, 2));
    uint32_t rc = (uint32_t)microkit_mr_get(0);
    int pass = (rc == WD_OK || rc == WD_ERR_NOENT);
    ipc_log(pass ? "[ipc_harness] PASS: watchdog STATUS\n"
                 : "[ipc_harness] FAIL: watchdog STATUS (unexpected reply)\n");
    return pass;
}
#endif /* IPC_HARNESS_HAS_WATCHDOG */

/* ── Summary reporter ─────────────────────────────────────────────────────── */

static void log_summary(int pass, int total)
{
    ipc_log("[ipc_harness] SUMMARY: ");
    ipc_log_digit(pass);
    ipc_log("/");
    ipc_log_digit(total);
    ipc_log(" tests passed\n");
}

/* ── Microkit entry points ────────────────────────────────────────────────── */

void init(void)
{
    ipc_log("[ipc_harness] PD online — running IPC test sequence\n");

    int pass = 0, total = 0;

    total++; pass += test_eventbus_status();
    total++; pass += test_initagent_status();
#ifdef IPC_HARNESS_HAS_WATCHDOG
    total++; pass += test_watchdog_status();
#endif

    log_summary(pass, total);
    ipc_log("[ipc_harness] idle\n");
}

void notified(microkit_channel ch)
{
    (void)ch;
    ipc_log("[ipc_harness] WARN: unexpected notified — PD is idle\n");
}

microkit_msginfo_t protected(microkit_channel ch, microkit_msginfo_t msginfo)
{
    (void)ch; (void)msginfo;
    ipc_log("[ipc_harness] WARN: unexpected protected — PD is idle\n");
    microkit_mr_set(0, 0xDEAD);
    return microkit_msginfo_new(0, 1);
}

#else /* !CONFIG_IPC_HARNESS */

/* ── Stub: no-op when CONFIG_IPC_HARNESS is not set ──────────────────────── */

void init(void)
{
    microkit_dbg_puts("[ipc_harness] disabled (build with CONFIG_IPC_HARNESS=1)\n");
}

void notified(microkit_channel ch) { (void)ch; }

microkit_msginfo_t protected(microkit_channel ch, microkit_msginfo_t msginfo)
{
    (void)ch; (void)msginfo;
    microkit_mr_set(0, 0xDEAD);
    return microkit_msginfo_new(0, 1);
}

#endif /* CONFIG_IPC_HARNESS */
