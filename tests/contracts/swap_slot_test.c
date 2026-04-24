/*
 * agentOS — swap_slot PD contract test (notification-only)
 * No IPC opcodes — tests verify struct definitions, error codes, and constants.
 * Notification status codes: MSG_VIBE_SLOT_READY, MSG_VIBE_SLOT_FAILED, MSG_VIBE_SLOT_HEALTHY
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/swap_slot_contract.h"

void run_swap_slot_tests(microkit_channel ch) {
    (void)ch;  /* notification-only PD, no IPC channel needed */
    TEST_SECTION("swap_slot");

    /* Verify error code values */
    ASSERT_TRUE(SWAP_SLOT_OK == 0, "swap slot ok == 0");
    ASSERT_TRUE(SWAP_SLOT_ERR_VERIFY_FAIL == 1, "swap slot err verify fail == 1");
    ASSERT_TRUE(SWAP_SLOT_ERR_BAD_WASM == 2, "swap slot err bad wasm == 2");
    ASSERT_TRUE(SWAP_SLOT_ERR_CAP_DENIED == 3, "swap slot err cap denied == 3");
    ASSERT_TRUE(SWAP_SLOT_ERR_OOM == 4, "swap slot err oom == 4");

    /* Verify notification status opcodes exist */
    ASSERT_TRUE(MSG_VIBE_SLOT_READY == 0x0601, "vibe slot ready == 0x0601");
    ASSERT_TRUE(MSG_VIBE_SLOT_FAILED == 0x0602, "vibe slot failed == 0x0602");
    ASSERT_TRUE(MSG_VIBE_SLOT_HEALTHY == 0x0603, "vibe slot healthy == 0x0603");

    /* Verify struct sizes are sane (packed structs should be small) */
    ASSERT_TRUE(sizeof(struct swap_slot_notify_ready) > 0, "notify ready struct exists");
    ASSERT_TRUE(sizeof(struct swap_slot_notify_failed) > 0, "notify failed struct exists");
    ASSERT_TRUE(sizeof(struct swap_slot_notify_healthy) > 0, "notify healthy struct exists");
}
