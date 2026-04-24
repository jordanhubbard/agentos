/*
 * agentOS — vibeos PD contract test
 * Covered opcodes: MSG_VIBEOS_CREATE, MSG_VIBEOS_DESTROY, MSG_VIBEOS_STATUS,
 *   MSG_VIBEOS_LIST, MSG_VIBEOS_BIND_DEVICE, MSG_VIBEOS_UNBIND_DEVICE,
 *   MSG_VIBEOS_SNAPSHOT, MSG_VIBEOS_RESTORE, MSG_VIBEOS_MIGRATE,
 *   MSG_VIBEOS_BOOT, MSG_VIBEOS_LOAD_MODULE, MSG_VIBEOS_CHECK_SERVICE_EXISTS,
 *   MSG_VIBEOS_CONFIGURE
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/vibeos_contract.h"

void run_vibeos_tests(microkit_channel ch) {
    TEST_SECTION("vibeos");

    /* STATUS — should always succeed */
    ASSERT_IPC_OK(ch, MSG_VIBEOS_STATUS, "vibeos status");

    /* LIST — should always succeed */
    ASSERT_IPC_OK(ch, MSG_VIBEOS_LIST, "vibeos list");

    /* CREATE — no setup, expect INVAL */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_VIBEOS_CREATE, VIBEOS_ERR_INVAL, "vibeos create no args");

    /* DESTROY — bogus id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VIBEOS_DESTROY, VIBEOS_ERR_NOT_FOUND, "vibeos destroy bogus id");

    /* BIND_DEVICE — bogus id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VIBEOS_BIND_DEVICE, VIBEOS_ERR_NOT_FOUND, "vibeos bind device bogus");

    /* UNBIND_DEVICE — bogus id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VIBEOS_UNBIND_DEVICE, VIBEOS_ERR_NOT_FOUND, "vibeos unbind device bogus");

    /* SNAPSHOT — bogus id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VIBEOS_SNAPSHOT, VIBEOS_ERR_NOT_FOUND, "vibeos snapshot bogus id");

    /* RESTORE — bogus id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VIBEOS_RESTORE, VIBEOS_ERR_NOT_FOUND, "vibeos restore bogus id");

    /* MIGRATE — bogus id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VIBEOS_MIGRATE, VIBEOS_ERR_NOT_FOUND, "vibeos migrate bogus id");

    /* BOOT — bogus id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VIBEOS_BOOT, VIBEOS_ERR_NOT_FOUND, "vibeos boot bogus id");

    /* LOAD_MODULE — no setup */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_VIBEOS_LOAD_MODULE, VIBEOS_ERR_INVAL, "vibeos load module no args");

    /* CHECK_SERVICE_EXISTS — bogus name */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_VIBEOS_CHECK_SERVICE_EXISTS, VIBEOS_ERR_NOT_FOUND, "vibeos check service bogus");

    /* CONFIGURE — bogus id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VIBEOS_CONFIGURE, VIBEOS_ERR_NOT_FOUND, "vibeos configure bogus id");

    /* Verify error code values */
    ASSERT_TRUE(VIBEOS_OK == 0, "vibeos ok == 0");
}
