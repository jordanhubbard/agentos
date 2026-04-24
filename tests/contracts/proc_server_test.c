/*
 * agentOS — proc_server PD contract test
 * Covered opcodes: OP_PROC_SPAWN, OP_PROC_EXIT, OP_PROC_WAIT, OP_PROC_STATUS,
 *   OP_PROC_LIST, OP_PROC_KILL, OP_PROC_SETCAP
 * NOTE: agentos.h redefines OP_PROC_* to 0xD0-0xD6, overriding contract header values
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/proc_server_contract.h"

void run_proc_server_tests(microkit_channel ch) {
    TEST_SECTION("proc_server");

    /* LIST — should always succeed */
    ASSERT_IPC_OK(ch, OP_PROC_LIST, "proc list");

    /* STATUS — bogus pid */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_PROC_STATUS, PROC_ERR_NOT_FOUND, "proc status bogus pid");

    /* SPAWN — no binary, expect INVAL */
    ASSERT_IPC_OK_OR_ERR(ch, OP_PROC_SPAWN, PROC_ERR_INVAL, "proc spawn no binary");

    /* KILL — bogus pid */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_PROC_KILL, PROC_ERR_NOT_FOUND, "proc kill bogus pid");

    /* WAIT — bogus pid */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_PROC_WAIT, PROC_ERR_NOT_FOUND, "proc wait bogus pid");

    /* EXIT — expect OK (self-exit) or INVAL */
    ASSERT_IPC_OK_OR_ERR(ch, OP_PROC_EXIT, PROC_ERR_INVAL, "proc exit");

    /* SETCAP — bogus pid */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_PROC_SETCAP, PROC_ERR_NOT_FOUND, "proc setcap bogus pid");

    /* Verify state constants */
    ASSERT_TRUE(PROC_STATE_FREE == 0, "proc state free == 0");
}
