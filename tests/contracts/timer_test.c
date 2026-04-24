/*
 * agentOS — timer PD contract test
 * Covered opcodes: TIMER_OP_CREATE, TIMER_OP_DESTROY, TIMER_OP_START,
 *   TIMER_OP_STOP, TIMER_OP_STATUS, TIMER_OP_CONFIGURE, TIMER_OP_SET_RTC,
 *   TIMER_OP_GET_RTC
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/timer_contract.h"

void run_timer_tests(microkit_channel ch) {
    TEST_SECTION("timer");

    /* STATUS — should always succeed */
    ASSERT_IPC_OK(ch, TIMER_OP_STATUS, "timer status");

    /* GET_RTC — should succeed or return not-impl */
    ASSERT_IPC_OK_OR_ERR(ch, TIMER_OP_GET_RTC, TIMER_ERR_NOT_IMPL, "timer get rtc");

    /* CREATE — expect OK or no slot */
    ASSERT_IPC_OK_OR_ERR(ch, TIMER_OP_CREATE, TIMER_ERR_NO_SLOT, "timer create");

    /* DESTROY — bogus slot */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, TIMER_OP_DESTROY, TIMER_ERR_BAD_SLOT, "timer destroy bogus slot");

    /* START — bogus slot */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, TIMER_OP_START, TIMER_ERR_BAD_SLOT, "timer start bogus slot");

    /* STOP — bogus slot */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, TIMER_OP_STOP, TIMER_ERR_BAD_SLOT, "timer stop bogus slot");

    /* CONFIGURE — bogus period */
    seL4_SetMR(1, 0);
    ASSERT_IPC_ERR(ch, TIMER_OP_CONFIGURE, TIMER_ERR_BAD_PERIOD, "timer configure zero period");

    /* SET_RTC — expect OK or not-impl */
    ASSERT_IPC_OK_OR_ERR(ch, TIMER_OP_SET_RTC, TIMER_ERR_NOT_IMPL, "timer set rtc");

    /* Verify constants */
    ASSERT_TRUE(TIMER_PD_CONTRACT_VERSION == 1, "timer contract version == 1");
    ASSERT_TRUE(TIMER_MAX_TIMERS == 8, "timer max timers == 8");
}
