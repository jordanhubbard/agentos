/*
 * agentOS — spawn_server PD contract test
 * Covered opcodes: OP_SPAWN_LAUNCH, OP_SPAWN_KILL, OP_SPAWN_STATUS,
 *   OP_SPAWN_LIST, OP_SPAWN_HEALTH
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/spawn_server_contract.h"

void run_spawn_server_tests(microkit_channel ch) {
    TEST_SECTION("spawn_server");

    /* HEALTH — should always succeed */
    ASSERT_IPC_OK(ch, OP_SPAWN_HEALTH, "spawn health");

    /* LIST — should always succeed */
    ASSERT_IPC_OK(ch, OP_SPAWN_LIST, "spawn list");

    /* STATUS — bogus app id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_SPAWN_STATUS, SPAWN_ERR_NOT_FOUND, "spawn status bogus id");

    /* LAUNCH — no binary setup, expect INVAL or VFS error */
    ASSERT_IPC_OK_OR_ERR(ch, OP_SPAWN_LAUNCH, SPAWN_ERR_INVAL, "spawn launch no binary");

    /* KILL — bogus app id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_SPAWN_KILL, SPAWN_ERR_NOT_FOUND, "spawn kill bogus id");

    /* Verify version constant */
    ASSERT_TRUE(SPAWN_VERSION == 1, "spawn version == 1");
}
