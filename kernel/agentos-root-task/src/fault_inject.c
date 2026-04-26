/*
 * agentOS Fault Injection Protection Domain
 *
 * Passive PD (priority 85).  Accepts OP_FAULT_INJECT commands from the
 * monitor (or a CI test harness) to deliberately trigger specific fault
 * conditions inside a target agent slot, then verifies the watchdog and
 * fault_handler recovery paths respond correctly.
 *
 * Supported fault types (MR2 = fault_kind):
 *   FAULT_NULL_DEREF     (0x01) — write to NULL pointer → seL4 VM fault
 *   FAULT_STACK_OVF      (0x02) — deep recursive call → stack overflow cap fault
 *   FAULT_QUOTA_EXCEEDED (0x03) — allocate past memory quota → alloc failure
 *   FAULT_IPC_TIMEOUT    (0x04) — PPC into non-replying target → timeout
 *   FAULT_UNALIGNED_MEM  (0x05) — unaligned memory access → alignment fault
 *
 * IPC protocol (MR layout):
 *   MR0 = opcode   (OP_FAULT_INJECT = 0xF0)
 *   MR1 = slot_id  (target agent slot, 0-based)
 *   MR2 = fault_kind
 *   MR3 = flags    (FAULT_FLAG_VERIFY_RECOVERY = 0x01 — wait for watchdog restart)
 *
 * Return MR layout:
 *   MR0 = result   (FAULT_RESULT_OK / FAULT_RESULT_ERROR / FAULT_RESULT_TIMEOUT)
 *   MR1 = ticks_to_recovery  (valid when FAULT_FLAG_VERIFY_RECOVERY set)
 *   MR2 = trace_event_id     (trace_recorder event id for the fault, if recorded)
 *
 * CI usage:
 *   1. Boot QEMU with fault_inject PD included in agentos.system
 *   2. Monitor PD sends OP_FAULT_INJECT (slot=1, FAULT_NULL_DEREF, VERIFY_RECOVERY)
 *   3. fault_inject triggers null deref in slot 1's WASM sandbox
 *   4. seL4 delivers a VM fault to fault_handler PD
 *   5. fault_handler PPCs monitor; monitor triggers watchdog restart
 *   6. fault_inject polls for slot RUNNING state, records ticks_to_recovery
 *   7. Returns FAULT_RESULT_OK if recovery completed within MAX_RECOVERY_TICKS
 *   8. CI fails if FAULT_RESULT_TIMEOUT or recovery took > threshold
 *
 * Integration with trace_recorder:
 *   fault_inject publishes a TRACE_EVENT_FAULT_INJECT event before triggering
 *   the fault, and a TRACE_EVENT_FAULT_RECOVERY after confirming recovery.
 *   trace_recorder captures both to the JSONL log for post-run analysis.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* ── Opcodes ────────────────────────────────────────────────────────────── */

#define OP_FAULT_INJECT       0xF0   /* trigger a fault in target slot      */
#define OP_FAULT_STATUS       0xF1   /* query last injection result          */
#define OP_FAULT_RESET        0xF2   /* reset injection state machine        */

/* Fault kinds */
#define FAULT_NULL_DEREF      0x01
#define FAULT_STACK_OVF       0x02
#define FAULT_QUOTA_EXCEEDED  0x03
#define FAULT_IPC_TIMEOUT     0x04
#define FAULT_UNALIGNED_MEM   0x05

/* Flags */
#define FAULT_FLAG_VERIFY_RECOVERY  0x01   /* wait for watchdog restart */
#define FAULT_FLAG_EXPECT_NO_CRASH  0x02   /* fault should be handled gracefully */

/* Results */
#define FAULT_RESULT_OK       0x00   /* fault triggered + recovery confirmed */
#define FAULT_RESULT_ERROR    0x01   /* could not trigger fault               */
#define FAULT_RESULT_TIMEOUT  0x02   /* recovery did not occur within limit   */
#define FAULT_RESULT_NO_CRASH 0x03   /* expected crash did not occur (test bug) */

/* Recovery timeout */
#define MAX_RECOVERY_TICKS    100    /* ~10s at 100ms tick; CI threshold      */

/* Trace event types for trace_recorder integration */
#define TRACE_EVENT_FAULT_INJECT    0x20
#define TRACE_EVENT_FAULT_RECOVERY  0x21

/* ── Channel assignment (must match agentos.system) ────────────────────── */
/* fault_inject is channel 70 from monitor; passive (pp=true on monitor end) */
#define CH_FAULT_INJECT       1    /* from monitor's perspective */
#define MY_CH_MONITOR         1    /* monitor sends OP_FAULT_INJECT here */

/* ── State machine ──────────────────────────────────────────────────────── */

typedef enum {
    FI_IDLE,
    FI_INJECTING,
    FI_WAITING_CRASH,
    FI_WAITING_RECOVERY,
    FI_DONE,
} FaultInjectState;

static struct {
    FaultInjectState state;
    uint32_t         target_slot;
    uint8_t          fault_kind;
    uint8_t          flags;
    uint32_t         inject_tick;
    uint32_t         crash_tick;
    uint32_t         recovery_tick;
    uint32_t         trace_event_id;
    uint8_t          last_result;
} s_fi = {0};

static uint32_t s_tick = 0;  /* driven by monitor notify */

/* ── Fault trigger helpers ──────────────────────────────────────────────── */

/*
 * Each helper writes a WASM-level instruction into the target slot's
 * shared control page that the slot's WASM host (wasm3_host.c) will
 * execute on next tick.  The slot's WASM sandbox traps → seL4 fault.
 */

/* Shared control page layout (per slot, mapped w from fault_inject) */
typedef struct __attribute__((packed)) {
    uint8_t  fault_req;      /* FAULT_* code to execute; 0 = idle */
    uint8_t  _pad[3];
    uint32_t stack_depth;    /* for FAULT_STACK_OVF: recursion depth */
    uint64_t fault_addr;     /* for FAULT_UNALIGNED_MEM: target addr */
    uint8_t  done;           /* set by wasm_host when fault triggered */
} SlotFaultCtrl;

/* One control page per slot (mapped at fault_ctrl_N vaddr) */
extern uintptr_t fault_ctrl_0;
extern uintptr_t fault_ctrl_1;
extern uintptr_t fault_ctrl_2;
extern uintptr_t fault_ctrl_3;

static SlotFaultCtrl *get_fault_ctrl(uint32_t slot) {
    switch (slot) {
        case 0: return (SlotFaultCtrl *)fault_ctrl_0;
        case 1: return (SlotFaultCtrl *)fault_ctrl_1;
        case 2: return (SlotFaultCtrl *)fault_ctrl_2;
        case 3: return (SlotFaultCtrl *)fault_ctrl_3;
        default: return NULL;
    }
}

static bool trigger_fault(uint32_t slot, uint8_t fault_kind) {
    SlotFaultCtrl *ctrl = get_fault_ctrl(slot);
    if (!ctrl) {
        sel4_dbg_puts("[fault_inject] invalid slot\n");
        return false;
    }
    ctrl->done      = 0;
    ctrl->fault_req = fault_kind;
    if (fault_kind == FAULT_STACK_OVF)
        ctrl->stack_depth = 65536;  /* guaranteed overflow on 64 KB stack */
    if (fault_kind == FAULT_UNALIGNED_MEM)
        ctrl->fault_addr = 0x1001;  /* odd address, forces alignment fault */

    sel4_dbg_puts("[fault_inject] fault triggered in slot ");
    char s[4] = { '0' + (char)(slot % 10), '\0' };
    sel4_dbg_puts(s);
    sel4_dbg_puts(" kind=0x");
    char k[4] = { "0123456789ABCDEF"[fault_kind >> 4],
                  "0123456789ABCDEF"[fault_kind & 0xF], '\0' };
    sel4_dbg_puts(k);
    sel4_dbg_puts("\n");
    return true;
}

/* ── Trace recorder integration ─────────────────────────────────────────── */

static uint32_t emit_trace_event(uint8_t kind, uint32_t slot, uint8_t fault_kind) {
    IPC_STUB_LOCALS
    static uint32_t s_trace_seq = 0;
    uint32_t seq = ++s_trace_seq;
    /* Notify trace_recorder via EventBus publish */
    rep_u32(rep, 0, kind);
    rep_u32(rep, 4, slot);
    rep_u32(rep, 8, fault_kind);
    rep_u32(rep, 12, seq);
    /* E5-S8: ppcall stubbed */
    return seq;
}

/* ── Recovery verification ──────────────────────────────────────────────── */

static bool slot_is_running(uint32_t slot) {
    IPC_STUB_LOCALS
    /* Query monitor for slot state via OP_SLOT_STATUS */
    rep_u32(rep, 0, slot);
    /* E5-S8: ppcall stubbed */
    return msg_u32(req, 0) == SLOT_STATE_RUNNING;
}

/* ── IPC handler ─────────────────────────────────────────────────────────── */

static void fault_inject_pd_init(void) {
    memset(&s_fi, 0, sizeof(s_fi));
    s_fi.state = FI_IDLE;
    sel4_dbg_puts("[fault_inject] PD online (priority 85)\n");
}

static uint32_t fault_inject_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint64_t op        = msg_u32(req, 0);
    uint32_t slot      = (uint32_t)msg_u32(req, 4);
    uint8_t  fkind     = (uint8_t) msg_u32(req, 8);
    uint8_t  flags     = (uint8_t) msg_u32(req, 12);

    switch (op) {

    case OP_FAULT_INJECT: {
        if (s_fi.state != FI_IDLE) {
            sel4_dbg_puts("[fault_inject] busy — reset first\n");
            rep_u32(rep, 0, FAULT_RESULT_ERROR);
            rep->length = 4;
        return SEL4_ERR_OK;
        }
        s_fi.target_slot    = slot;
        s_fi.fault_kind     = fkind;
        s_fi.flags          = flags;
        s_fi.inject_tick    = s_tick;
        s_fi.crash_tick     = 0;
        s_fi.recovery_tick  = 0;
        s_fi.last_result    = FAULT_RESULT_ERROR;

        /* Emit pre-fault trace event */
        s_fi.trace_event_id = emit_trace_event(TRACE_EVENT_FAULT_INJECT, slot, fkind);

        if (!trigger_fault(slot, fkind)) {
            rep_u32(rep, 0, FAULT_RESULT_ERROR);
            rep_u32(rep, 4, 0);
            rep_u32(rep, 8, 0);
            rep->length = 12;
        return SEL4_ERR_OK;
        }
        s_fi.state = FI_WAITING_CRASH;

        if (flags & FAULT_FLAG_VERIFY_RECOVERY) {
            /*
             * Poll for recovery in notify() handler (driven by monitor tick).
             * Return FAULT_RESULT_OK immediately so monitor is not blocked;
             * actual recovery status is polled via OP_FAULT_STATUS.
             */
            rep_u32(rep, 0, FAULT_RESULT_OK);
            rep_u32(rep, 4, 0);
            rep_u32(rep, 8, s_fi.trace_event_id);
            rep->length = 12;
        return SEL4_ERR_OK;
        }
        s_fi.state = FI_DONE;
        s_fi.last_result = FAULT_RESULT_OK;
        rep_u32(rep, 0, FAULT_RESULT_OK);
        rep_u32(rep, 4, 0);
        rep_u32(rep, 8, s_fi.trace_event_id);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    case OP_FAULT_STATUS: {
        /* Return current state: result, ticks_to_recovery, trace_event_id */
        rep_u32(rep, 0, s_fi.last_result);
        uint32_t ticks = (s_fi.recovery_tick > s_fi.inject_tick)
                         ? (s_fi.recovery_tick - s_fi.inject_tick) : 0;
        rep_u32(rep, 4, ticks);
        rep_u32(rep, 8, s_fi.trace_event_id);
        rep_u32(rep, 12, (uint64_t)s_fi.state);
        rep->length = 16;
        return SEL4_ERR_OK;
    }

    case OP_FAULT_RESET: {
        memset(&s_fi, 0, sizeof(s_fi));
        s_fi.state = FI_IDLE;
        sel4_dbg_puts("[fault_inject] state reset\n");
        rep_u32(rep, 0, FAULT_RESULT_OK);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    default:
        rep_u32(rep, 0, 0xDEAD);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
}

static void fault_inject_pd_notified(uint32_t ch) {
    /* Monitor sends a tick notify each watchdog cycle */
    s_tick++;

    if (s_fi.state == FI_WAITING_CRASH) {
        /* Check if slot has crashed (no longer RUNNING) */
        if (!slot_is_running(s_fi.target_slot)) {
            s_fi.crash_tick = s_tick;
            s_fi.state      = FI_WAITING_RECOVERY;
            sel4_dbg_puts("[fault_inject] crash confirmed, waiting for recovery\n");
        } else if (s_tick - s_fi.inject_tick > MAX_RECOVERY_TICKS) {
            sel4_dbg_puts("[fault_inject] TIMEOUT: slot did not crash\n");
            s_fi.last_result = FAULT_RESULT_NO_CRASH;
            s_fi.state       = FI_DONE;
        }
        return;
    }

    if (s_fi.state == FI_WAITING_RECOVERY) {
        /* Check if watchdog has restarted the slot */
        if (slot_is_running(s_fi.target_slot)) {
            s_fi.recovery_tick = s_tick;
            s_fi.last_result   = FAULT_RESULT_OK;
            s_fi.state         = FI_DONE;

            /* Emit post-recovery trace event */
            emit_trace_event(TRACE_EVENT_FAULT_RECOVERY,
                             s_fi.target_slot, s_fi.fault_kind);

            sel4_dbg_puts("[fault_inject] recovery confirmed\n");
        } else if (s_tick - s_fi.crash_tick > MAX_RECOVERY_TICKS) {
            sel4_dbg_puts("[fault_inject] TIMEOUT: slot did not recover\n");
            s_fi.last_result = FAULT_RESULT_TIMEOUT;
            s_fi.state       = FI_DONE;
        }
        return;
    }
    (void)ch;
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void fault_inject_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    fault_inject_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, fault_inject_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
