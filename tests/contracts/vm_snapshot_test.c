/*
 * agentOS — vm_snapshot PD contract test
 * Covered opcodes: OP_VM_SNAPSHOT_REQ (OP_VM_SNAPSHOT), OP_VM_RESTORE_REQ (OP_VM_RESTORE)
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/vm_snapshot_contract.h"

void run_vm_snapshot_tests(microkit_channel ch) {
    TEST_SECTION("vm_snapshot");

    /* SNAPSHOT — bogus vm id, expect error */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_OK_OR_ERR(ch, OP_VM_SNAPSHOT_REQ, SNAP_ERR, "vm snapshot bogus id");

    /* RESTORE — bogus vm id, expect error */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_OK_OR_ERR(ch, OP_VM_RESTORE_REQ, SNAP_ERR, "vm restore bogus id");

    /* Verify error codes */
    ASSERT_TRUE(SNAP_OK == 0x00, "snap ok == 0x00");
    ASSERT_TRUE(SNAP_ERR == 0xFF, "snap err == 0xFF");
    ASSERT_TRUE(SNAP_ERR_HASH == 0xFE, "snap err hash == 0xFE");
}
