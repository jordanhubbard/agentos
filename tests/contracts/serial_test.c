/*
 * agentOS — serial PD contract test
 * Covered opcodes: MSG_SERIAL_OPEN, MSG_SERIAL_CLOSE, MSG_SERIAL_WRITE,
 *   MSG_SERIAL_READ, MSG_SERIAL_STATUS, MSG_SERIAL_CONFIGURE
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/serial_contract.h"

void run_serial_tests(microkit_channel ch) {
    TEST_SECTION("serial");

    /* STATUS — should always succeed */
    ASSERT_IPC_OK(ch, MSG_SERIAL_STATUS, "serial status");

    /* OPEN — expect OK or no slots */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_SERIAL_OPEN, SERIAL_ERR_NO_SLOTS, "serial open");

    /* CLOSE — bogus slot */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_SERIAL_CLOSE, SERIAL_ERR_BAD_SLOT, "serial close bogus slot");

    /* WRITE — bogus slot */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_SERIAL_WRITE, SERIAL_ERR_BAD_SLOT, "serial write bogus slot");

    /* READ — bogus slot */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_SERIAL_READ, SERIAL_ERR_BAD_SLOT, "serial read bogus slot");

    /* CONFIGURE — bogus slot */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_SERIAL_CONFIGURE, SERIAL_ERR_BAD_SLOT, "serial configure bogus slot");

    /* Verify constants */
    ASSERT_TRUE(SERIAL_MAX_WRITE_BYTES == 256, "serial max write bytes == 256");
    ASSERT_TRUE(SERIAL_MAX_CLIENTS == 8, "serial max clients == 8");
}
