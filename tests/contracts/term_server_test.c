/*
 * agentOS — term_server PD contract test
 * Covered opcodes: OP_TERM_OPENPTY, OP_TERM_RESIZE, OP_TERM_WRITE,
 *   OP_TERM_READ, OP_TERM_CLOSEPTY, OP_TERM_STATUS
 * NOTE: agentos.h redefines OP_TERM_* to 0xA0-0xA5, overriding contract header values
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/term_server_contract.h"

void run_term_server_tests(microkit_channel ch) {
    TEST_SECTION("term_server");

    /* STATUS — should always succeed */
    ASSERT_IPC_OK(ch, OP_TERM_STATUS, "term status");

    /* OPENPTY — expect OK or no ptys */
    ASSERT_IPC_OK_OR_ERR(ch, OP_TERM_OPENPTY, TERM_ERR_NO_PTYS, "term openpty");

    /* RESIZE — bogus pty */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_TERM_RESIZE, TERM_ERR_NOT_FOUND, "term resize bogus pty");

    /* WRITE — bogus pty */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_TERM_WRITE, TERM_ERR_NOT_FOUND, "term write bogus pty");

    /* READ — bogus pty */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_TERM_READ, TERM_ERR_NOT_FOUND, "term read bogus pty");

    /* CLOSEPTY — bogus pty */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_TERM_CLOSEPTY, TERM_ERR_NOT_FOUND, "term closepty bogus pty");

    /* Verify constants */
    ASSERT_TRUE(TERM_RING_MAGIC == 0x52494E47, "term ring magic == RING");
    ASSERT_TRUE(TERM_RING_DATA_BYTES == 2032, "term ring data bytes == 2032");
    ASSERT_TRUE(TERM_PTY_SLOTS == 4, "term pty slots == 4");
    ASSERT_TRUE(TERM_SHMEM_TOTAL == 16384, "term shmem total == 16384");
}
