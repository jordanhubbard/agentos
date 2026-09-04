/*
 * agentOS — net PD contract test
 * Covered opcodes: MSG_NET_OPEN, MSG_NET_CLOSE, MSG_NET_SEND, MSG_NET_RECV,
 *   MSG_NET_DEV_STATUS, MSG_NET_CONFIGURE, MSG_NET_FILTER_ADD, MSG_NET_FILTER_REMOVE,
 *   MSG_NET_SOCKET_OPEN, MSG_NET_SOCKET_CLOSE, MSG_NET_SOCKET_CONNECT,
 *   MSG_NET_SOCKET_BIND, MSG_NET_SOCKET_LISTEN, MSG_NET_SOCKET_ACCEPT,
 *   MSG_NET_SOCKET_SET_OPT
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/net_contract.h"
#include "../../contracts/net-service/interface.h"

void run_net_tests(microkit_channel ch) {
    TEST_SECTION("net");

    ASSERT_TRUE(NET_PD_CONTRACT_VERSION == 2u, "net contract version == 2");
    ASSERT_TRUE(NET_SVC_OP_RAW_OPEN == MSG_NET_OPEN &&
                NET_SVC_OP_RAW_SEND == MSG_NET_SEND &&
                NET_SVC_OP_RAW_RECV == MSG_NET_RECV,
                "canonical and legacy net opcodes agree");
    ASSERT_TRUE(NET_SVC_EVENT_RX_READY == 0x2110u,
                "net RX-ready event opcode is stable");
    ASSERT_TRUE(sizeof(net_svc_raw_open_reply_t) == 20u,
                "canonical raw OPEN reply wire size");
    ASSERT_TRUE(sizeof(struct net_reply_open) == 20u,
                "net OPEN reply includes packed shmem offset and MAC");
    ASSERT_TRUE(NET_SHMEM_SLOT_BASE + NET_MAX_CLIENTS * NET_SHMEM_SLOT_BYTES
                    <= NET_SHMEM_BYTES,
                "net contract slots fit shared memory");

    /* STATUS — should always succeed */
    ASSERT_IPC_OK(ch, MSG_NET_DEV_STATUS, "net dev status");

    /* OPEN — no shmem, expect INVAL or error */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_NET_OPEN, NET_ERR_NO_SLOTS, "net open without slot");

    /* CLOSE — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_NET_CLOSE, NET_ERR_BAD_HANDLE, "net close bogus handle");

    /* SEND — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_NET_SEND, NET_ERR_BAD_HANDLE, "net send bogus handle");

    /* RECV — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_NET_RECV, NET_ERR_BAD_HANDLE, "net recv bogus handle");

    /* CONFIGURE — bogus iface */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_NET_CONFIGURE, NET_ERR_BAD_IFACE, "net configure bogus iface");

    /* FILTER_ADD — expect full or error */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_NET_FILTER_ADD, NET_ERR_FILTER_FULL, "net filter add");

    /* FILTER_REMOVE — bogus filter id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_NET_FILTER_REMOVE, NET_ERR_BAD_FILTER_ID, "net filter remove bogus");

    /* SOCKET_OPEN */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_NET_SOCKET_OPEN, NET_ERR_NO_SLOTS, "net socket open");

    /* SOCKET_CLOSE — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_NET_SOCKET_CLOSE, NET_ERR_BAD_HANDLE, "net socket close bogus");

    /* SOCKET_CONNECT — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_NET_SOCKET_CONNECT, NET_ERR_BAD_HANDLE, "net socket connect bogus");

    /* SOCKET_BIND — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_NET_SOCKET_BIND, NET_ERR_BAD_HANDLE, "net socket bind bogus");

    /* SOCKET_LISTEN — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_NET_SOCKET_LISTEN, NET_ERR_BAD_HANDLE, "net socket listen bogus");

    /* SOCKET_ACCEPT — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_NET_SOCKET_ACCEPT, NET_ERR_BAD_HANDLE, "net socket accept bogus");

    /* SOCKET_SET_OPT — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_NET_SOCKET_SET_OPT, NET_ERR_BAD_HANDLE, "net socket set opt bogus");
}
