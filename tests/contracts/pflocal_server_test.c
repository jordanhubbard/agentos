/*
 * agentOS — pflocal_server PD contract test
 * Covered opcodes: OP_PFLOCAL_SOCKET, OP_PFLOCAL_BIND, OP_PFLOCAL_LISTEN,
 *   OP_PFLOCAL_CONNECT, OP_PFLOCAL_ACCEPT, OP_PFLOCAL_SEND, OP_PFLOCAL_RECV,
 *   OP_PFLOCAL_CLOSE, OP_PFLOCAL_STATUS
 * NOTE: agentos.h redefines OP_PFLOCAL_* to 0xE1-0xE9, overriding contract header values
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/pflocal_server_contract.h"

void run_pflocal_server_tests(microkit_channel ch) {
    TEST_SECTION("pflocal_server");

    /* STATUS — should always succeed */
    ASSERT_IPC_OK(ch, OP_PFLOCAL_STATUS, "pflocal status");

    /* SOCKET — create a unix domain socket */
    ASSERT_IPC_OK_OR_ERR(ch, OP_PFLOCAL_SOCKET, PFLOCAL_ERR_NO_SOCKS, "pflocal socket create");

    /* BIND — bogus socket fd */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_PFLOCAL_BIND, PFLOCAL_ERR_NOT_FOUND, "pflocal bind bogus fd");

    /* LISTEN — bogus socket fd */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_PFLOCAL_LISTEN, PFLOCAL_ERR_NOT_FOUND, "pflocal listen bogus fd");

    /* CONNECT — bogus socket fd */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_PFLOCAL_CONNECT, PFLOCAL_ERR_NOT_FOUND, "pflocal connect bogus fd");

    /* ACCEPT — bogus socket fd */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_PFLOCAL_ACCEPT, PFLOCAL_ERR_NOT_FOUND, "pflocal accept bogus fd");

    /* SEND — bogus socket fd */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_PFLOCAL_SEND, PFLOCAL_ERR_NOT_FOUND, "pflocal send bogus fd");

    /* RECV — bogus socket fd */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_PFLOCAL_RECV, PFLOCAL_ERR_NOT_FOUND, "pflocal recv bogus fd");

    /* CLOSE — bogus socket fd */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_PFLOCAL_CLOSE, PFLOCAL_ERR_NOT_FOUND, "pflocal close bogus fd");

    /* Verify constants */
    ASSERT_TRUE(PFLOCAL_SOCK_SLOTS == 16, "pflocal sock slots == 16");
    ASSERT_TRUE(PFLOCAL_SLOT_BYTES == 4096, "pflocal slot bytes == 4096");
}
