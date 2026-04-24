/*
 * agentOS — power_mgr PD contract test
 * Covered opcodes: MSG_PWR_STATUS, MSG_PWR_DVFS_SET, MSG_PWR_SLEEP
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/power_mgr_contract.h"

void run_power_mgr_tests(microkit_channel ch) {
    TEST_SECTION("power_mgr");

    /* STATUS — should always succeed */
    ASSERT_IPC_OK(ch, MSG_PWR_STATUS, "power status");

    /* DVFS_SET — bogus frequency */
    seL4_SetMR(1, 0xFFFFFFFF);
    ASSERT_IPC_ERR(ch, MSG_PWR_DVFS_SET, PWR_ERR_BAD_FREQ, "dvfs set bogus freq");

    /* SLEEP — bogus level */
    seL4_SetMR(1, 0xFF);
    ASSERT_IPC_ERR(ch, MSG_PWR_SLEEP, PWR_ERR_BAD_LEVEL, "sleep bogus level");

    /* Verify state constants */
    ASSERT_TRUE(PWR_STATE_RUNNING == 0, "pwr state running == 0");
    ASSERT_TRUE(PWR_STATE_IDLE == 1, "pwr state idle == 1");
    ASSERT_TRUE(PWR_STATE_SUSPEND == 2, "pwr state suspend == 2");
}
