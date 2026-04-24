/*
 * agentOS — wg_net PD contract test
 * Covered opcodes: OP_WG_SET_PRIVKEY, OP_WG_ADD_PEER, OP_WG_REMOVE_PEER,
 *   OP_WG_SEND, OP_WG_RECV, OP_WG_STATUS, OP_WG_HEALTH
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/wg_net_contract.h"

void run_wg_net_tests(microkit_channel ch) {
    TEST_SECTION("wg_net");

    /* HEALTH — should always succeed */
    ASSERT_IPC_OK(ch, OP_WG_HEALTH, "wg health");

    /* STATUS — should always succeed */
    ASSERT_IPC_OK(ch, OP_WG_STATUS, "wg status");

    /* SET_PRIVKEY — should succeed or error */
    ASSERT_IPC_OK_OR_ERR(ch, OP_WG_SET_PRIVKEY, WG_ERR_CRYPTO, "wg set privkey");

    /* ADD_PEER — expect OK or full */
    ASSERT_IPC_OK_OR_ERR(ch, OP_WG_ADD_PEER, WG_ERR_FULL, "wg add peer");

    /* REMOVE_PEER — bogus peer */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_WG_REMOVE_PEER, WG_ERR_NOPEER, "wg remove bogus peer");

    /* SEND — no key set */
    ASSERT_IPC_OK_OR_ERR(ch, OP_WG_SEND, WG_ERR_NOKEY, "wg send no key");

    /* RECV — no key set */
    ASSERT_IPC_OK_OR_ERR(ch, OP_WG_RECV, WG_ERR_NOKEY, "wg recv no key");

    /* Verify error codes */
    ASSERT_TRUE(WG_OK == 0, "wg ok == 0");
    ASSERT_TRUE(WG_ERR_NOPEER == 1, "wg err nopeer == 1");
    ASSERT_TRUE(WG_ERR_FULL == 2, "wg err full == 2");
    ASSERT_TRUE(WG_ERR_CRYPTO == 3, "wg err crypto == 3");
    ASSERT_TRUE(WG_ERR_NOKEY == 4, "wg err nokey == 4");
}
