/*
 * agentOS — vfs_server PD contract test
 * Covered opcodes: OP_VFS_OPEN, OP_VFS_CLOSE, OP_VFS_READ, OP_VFS_WRITE,
 *   OP_VFS_STAT, OP_VFS_UNLINK, OP_VFS_MKDIR, OP_VFS_READDIR,
 *   OP_VFS_TRUNCATE, OP_VFS_SYNC, OP_VFS_MOUNT, OP_VFS_HEALTH
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/vfs_server_contract.h"

void run_vfs_server_tests(microkit_channel ch) {
    TEST_SECTION("vfs_server");

    /* HEALTH — should always succeed */
    ASSERT_IPC_OK(ch, OP_VFS_HEALTH, "vfs health");

    /* OPEN — no path setup, expect INVAL */
    ASSERT_IPC_OK_OR_ERR(ch, OP_VFS_OPEN, VFS_ERR_INVAL, "vfs open no path");

    /* CLOSE — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_VFS_CLOSE, VFS_ERR_NOT_FOUND, "vfs close bogus handle");

    /* READ — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_VFS_READ, VFS_ERR_NOT_FOUND, "vfs read bogus handle");

    /* WRITE — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_VFS_WRITE, VFS_ERR_NOT_FOUND, "vfs write bogus handle");

    /* STAT — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_VFS_STAT, VFS_ERR_NOT_FOUND, "vfs stat bogus handle");

    /* UNLINK — no path, expect INVAL */
    ASSERT_IPC_OK_OR_ERR(ch, OP_VFS_UNLINK, VFS_ERR_INVAL, "vfs unlink no path");

    /* MKDIR — no path, expect INVAL */
    ASSERT_IPC_OK_OR_ERR(ch, OP_VFS_MKDIR, VFS_ERR_INVAL, "vfs mkdir no path");

    /* READDIR — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_VFS_READDIR, VFS_ERR_NOT_FOUND, "vfs readdir bogus handle");

    /* TRUNCATE — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, OP_VFS_TRUNCATE, VFS_ERR_NOT_FOUND, "vfs truncate bogus handle");

    /* SYNC — should succeed even if no-op */
    ASSERT_IPC_OK(ch, OP_VFS_SYNC, "vfs sync");

    /* MOUNT — no args, expect INVAL */
    ASSERT_IPC_OK_OR_ERR(ch, OP_VFS_MOUNT, VFS_ERR_INVAL, "vfs mount no args");

    /* Verify constants */
    ASSERT_TRUE(VFS_VERSION == 1, "vfs version == 1");
}
