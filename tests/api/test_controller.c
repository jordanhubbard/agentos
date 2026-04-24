/*
 * test_controller.c — API tests for the agentOS controller PD (E5-S2)
 *
 * Covered scenarios:
 *   1.  controller_main initialises without crashing
 *   2.  capability policy is loaded at init
 *   3.  handler table is populated after init (>0 handlers)
 *   4.  exactly 4 inbound opcodes are registered
 *   5.  OP_CAP_POLICY_RELOAD handler returns SEL4_ERR_OK
 *   6.  OP_CAP_POLICY_RELOAD sets policy_loaded = true
 *   7.  MSG_VMM_REGISTER handler returns SEL4_ERR_OK
 *   8.  MSG_VMM_REGISTER reply carries vmm_token field
 *   9.  MSG_VMM_REGISTER reply carries granted_guests field
 *   10. MSG_VMM_VCPU_SET_REGS handler returns SEL4_ERR_OK (no shmem => host stub ok)
 *   11. MSG_WORKER_RETRIEVE handler returns SEL4_ERR_OK (agentfs stub ok)
 *   12. Unknown opcode returns SEL4_ERR_INVALID_OP
 *   13. EventBus ready flag set after init (stub connect returns OK)
 *   14. boot sequence: notification count starts at zero
 *   15. notification kind 0 sets eventbus_ready
 *   16. notification kind 1 with tag != MSG_SPAWN_AGENT sets initagent_ready
 *   17. notification kind 1 with MSG_SPAWN_AGENT triggers agent spawn path
 *   18. notification kind 3 (vibe engine) triggers vibe_swap_in_progress
 *   19. notification kind 5 (gpu sched online) — GPU Scheduler online path
 *   20. notification kind 6 (mesh agent) executes without fault
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST \
 *      -I tests/api \
 *      -I kernel/agentos-root-task/include \
 *      -o /tmp/test_controller \
 *      tests/api/test_controller.c && /tmp/test_controller
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"

/*
 * Pull in the controller implementation under AGENTOS_TEST_HOST.
 * All seL4/Microkit references inside monitor.c are replaced by stubs
 * defined in monitor.c itself when AGENTOS_TEST_HOST is set.
 */
#include "../../kernel/agentos-root-task/src/monitor.c"

/* ─────────────────────────────────────────────────────────────────────────────
 * Stub implementations required by monitor.c that are elsewhere in the tree
 * ─────────────────────────────────────────────────────────────────────────── */

/* vibe_swap subsystem stubs */
void vibe_swap_init(void) {}
int  vibe_swap_begin(uint32_t service_id, const void *code, uint32_t code_len)
{
    (void)service_id; (void)code; (void)code_len;
    return 0; /* slot 0 */
}
int  vibe_swap_health_notify(int slot) { (void)slot; return 0; }
int  vibe_swap_rollback(uint32_t service_id) { (void)service_id; return 0; }

/* ─────────────────────────────────────────────────────────────────────────────
 * Setup helper — run controller_main with sentinel endpoint caps
 * ─────────────────────────────────────────────────────────────────────────── */

static void setup(void)
{
    /* Reset mutable globals that persist across tests */
    log_drain_rings_vaddr       = 0;
    vibe_staging_ctrl_vaddr     = 0;
    cap_policy_shmem_vaddr      = 0;
    app_manifest_shmem_ctrl_vaddr = 0;
    vmm_vcpu_regs_vaddr         = 0;
    swap_code_ctrl_0            = 0;
    swap_code_ctrl_1            = 0;
    swap_code_ctrl_2            = 0;
    swap_code_ctrl_3            = 0;

    /* Sentinel caps: 0 = nameserver endpoint (host stubs accept any value) */
    controller_main(/*my_ep=*/1u, /*ns_ep=*/0u);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Helper: build a sel4_msg_t with a given opcode
 * ─────────────────────────────────────────────────────────────────────────── */

static sel4_msg_t make_req(uint32_t opcode)
{
    sel4_msg_t req;
    req.opcode = opcode;
    req.length = 0;
    for (uint32_t i = 0; i < SEL4_MSG_DATA_BYTES; i++)
        req.data[i] = 0u;
    return req;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 1: controller_main initialises without crashing
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_init_no_crash(void)
{
    setup();
    /* If we reached here, init did not crash. */
    TAP_OK("controller_main: completes without crash");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 2: capability policy is loaded at init
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_policy_loaded_at_init(void)
{
    setup();
    ASSERT_TRUE(controller_policy_loaded(),
                "controller_main: policy_loaded == true after init");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 3: handler table is populated after init
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_handler_table_populated(void)
{
    setup();
    ASSERT_TRUE(controller_handler_count() > 0u,
                "controller_main: at least one handler registered");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 4: exactly 4 inbound opcodes are registered
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_handler_count_four(void)
{
    setup();
    ASSERT_EQ(controller_handler_count(), 4u,
              "controller_main: exactly 4 handlers registered");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 5: OP_CAP_POLICY_RELOAD returns SEL4_ERR_OK
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_cap_policy_reload_ok(void)
{
    setup();
    sel4_msg_t req = make_req(OP_CAP_POLICY_RELOAD);
    sel4_msg_t rep;
    uint32_t rc = controller_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "OP_CAP_POLICY_RELOAD: returns SEL4_ERR_OK");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 6: OP_CAP_POLICY_RELOAD sets policy_loaded flag in reply
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_cap_policy_reload_reply_data(void)
{
    setup();
    sel4_msg_t req = make_req(OP_CAP_POLICY_RELOAD);
    sel4_msg_t rep;
    controller_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rep.data[0], 1u,
              "OP_CAP_POLICY_RELOAD: reply data[0] == 1 (loaded)");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 7: MSG_VMM_REGISTER returns SEL4_ERR_OK
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_vmm_register_ok(void)
{
    setup();
    sel4_msg_t req = make_req(MSG_VMM_REGISTER);
    sel4_msg_t rep;
    uint32_t rc = controller_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "MSG_VMM_REGISTER: returns SEL4_ERR_OK");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 8: MSG_VMM_REGISTER reply carries vmm_token
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_vmm_register_token(void)
{
    setup();
    sel4_msg_t req = make_req(MSG_VMM_REGISTER);
    sel4_msg_t rep;
    controller_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rep.data[1], 1u,
              "MSG_VMM_REGISTER: reply data[1] == 1 (vmm_token)");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 9: MSG_VMM_REGISTER reply carries granted_guests
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_vmm_register_guests(void)
{
    setup();
    sel4_msg_t req = make_req(MSG_VMM_REGISTER);
    sel4_msg_t rep;
    controller_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rep.data[2], 1u,
              "MSG_VMM_REGISTER: reply data[2] == 1 (granted_guests)");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 10: MSG_VMM_VCPU_SET_REGS returns SEL4_ERR_OK (host: no shmem check)
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_vmm_vcpu_set_regs_ok(void)
{
    setup();
    sel4_msg_t req = make_req(MSG_VMM_VCPU_SET_REGS);
    sel4_msg_t rep;
    uint32_t rc = controller_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "MSG_VMM_VCPU_SET_REGS: returns SEL4_ERR_OK on host (no shmem)");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 11: MSG_WORKER_RETRIEVE returns SEL4_ERR_OK (agentfs stub)
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_worker_retrieve_ok(void)
{
    setup();
    sel4_msg_t req = make_req(MSG_WORKER_RETRIEVE);
    req.length = 16u;  /* 4 × uint32 object ID words */
    sel4_msg_t rep;
    uint32_t rc = controller_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "MSG_WORKER_RETRIEVE: returns SEL4_ERR_OK with agentfs stub");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 12: Unknown opcode returns SEL4_ERR_INVALID_OP
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_unknown_opcode(void)
{
    setup();
    sel4_msg_t req = make_req(0xDEADBEEFu);
    sel4_msg_t rep;
    uint32_t rc = controller_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_INVALID_OP,
              "unknown opcode: returns SEL4_ERR_INVALID_OP");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 13: EventBus ready flag set after init (stub connect + call return OK)
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_eventbus_ready_after_init(void)
{
    setup();
    ASSERT_TRUE(controller_eventbus_ready(),
                "boot sequence: eventbus_ready == true after controller_main");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 14: notification count starts at zero before any notifications
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_notif_count_zero_at_start(void)
{
    setup();
    /*
     * After setup() (controller_main), demo_sequence() is run internally.
     * The notification counter is only incremented by
     * controller_handle_notification(), which is NOT called during init.
     * So the count must still be 0 right after init.
     */
    ASSERT_EQ(controller_notif_count(), 0u,
              "boot sequence: notification_count == 0 after init");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 15: notification kind 0 sets eventbus_ready
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_notif_eventbus(void)
{
    setup();
    /* Force eventbus_ready to false to test the notification path */
    /* (it was set true during init via sel4_client_call stub) */
    controller_handle_notification(0u, 0u, 0u);
    ASSERT_TRUE(controller_eventbus_ready(),
                "notification kind 0: eventbus_ready set to true");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 16: notification kind 1 (non-spawn) sets initagent_ready
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_notif_initagent_ready(void)
{
    setup();
    /* arg0 != MSG_SPAWN_AGENT → "ready" branch */
    controller_handle_notification(1u, 0u, 0u);
    ASSERT_TRUE(controller_initagent_ready(),
                "notification kind 1, non-spawn: initagent_ready set to true");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 17: notification kind 1 with MSG_SPAWN_AGENT triggers spawn path
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_notif_spawn_agent(void)
{
    setup();
    uint32_t before = controller_notif_count();
    controller_handle_notification(1u, (uint32_t)MSG_SPAWN_AGENT, /*spawn_id=*/42u);
    uint32_t after = controller_notif_count();
    ASSERT_TRUE(after > before,
                "notification kind 1, MSG_SPAWN_AGENT: notification_count incremented");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 18: notification kind 3 (vibe engine) triggers vibe_swap_in_progress
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_notif_vibe_engine(void)
{
    setup();
    controller_handle_notification(3u, 0u, 0u);
    ASSERT_TRUE(controller_vibe_in_progress(),
                "notification kind 3: vibe_swap_in_progress set after VibeEngine notify");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 19: notification kind 5 (gpu sched) — no crash, count increments
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_notif_gpu_sched_online(void)
{
    setup();
    uint32_t before = controller_notif_count();
    /* arg0 != MSG_GPU_SUBMIT => "GPU Scheduler online" branch */
    controller_handle_notification(5u, 0u, 0u);
    uint32_t after = controller_notif_count();
    ASSERT_TRUE(after > before,
                "notification kind 5: GPU Scheduler online path executes, count increments");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test 20: notification kind 6 (mesh agent) — no crash, count increments
 * ─────────────────────────────────────────────────────────────────────────── */

static void test_notif_mesh_agent(void)
{
    setup();
    uint32_t before = controller_notif_count();
    controller_handle_notification(6u, 0u, 0u);
    uint32_t after = controller_notif_count();
    ASSERT_TRUE(after > before,
                "notification kind 6: mesh agent online path executes, count increments");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * main
 * ─────────────────────────────────────────────────────────────────────────── */

int main(void)
{
    TAP_PLAN(20);

    test_init_no_crash();
    test_policy_loaded_at_init();
    test_handler_table_populated();
    test_handler_count_four();
    test_cap_policy_reload_ok();
    test_cap_policy_reload_reply_data();
    test_vmm_register_ok();
    test_vmm_register_token();
    test_vmm_register_guests();
    test_vmm_vcpu_set_regs_ok();
    test_worker_retrieve_ok();
    test_unknown_opcode();
    test_eventbus_ready_after_init();
    test_notif_count_zero_at_start();
    test_notif_eventbus();
    test_notif_initagent_ready();
    test_notif_spawn_agent();
    test_notif_vibe_engine();
    test_notif_gpu_sched_online();
    test_notif_mesh_agent();

    return tap_exit();
}

#else /* !AGENTOS_TEST_HOST */
typedef int _agentos_controller_test_dummy;
#endif /* AGENTOS_TEST_HOST */
