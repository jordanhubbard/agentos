/*
 * linux_vmm_test.c — contract tests for the LinuxVMM PD
 *
 * Covered opcodes:
 *   MSG_VM_CREATE                  — create a Linux VM
 *   MSG_VM_DESTROY                 — destroy a VM
 *   MSG_VM_STATUS                  — query VM status
 *   MSG_VM_LIST                    — list VMs
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_linux_vmm_tests(microkit_channel ch)
{
    TEST_SECTION("linux_vmm");

    /* LIST — enumerate Linux VMs */
    ASSERT_IPC_OK(ch, MSG_VM_LIST, "linux_vmm: LIST returns ok");

    /* STATUS — query bogus vm_id */
    microkit_mr_set(0, (uint64_t)MSG_VM_STATUS);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VM_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("linux_vmm: STATUS with bogus id returns ok/not-found/inval");
        } else {
            _tf_fail_point("linux_vmm: STATUS with bogus id returns ok/not-found/inval",
                           "unexpected error code");
        }
    }

    /* DESTROY — destroy bogus vm_id */
    microkit_mr_set(0, (uint64_t)MSG_VM_DESTROY);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VM_DESTROY, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("linux_vmm: DESTROY with bogus id returns not-found/inval");
        } else {
            _tf_fail_point("linux_vmm: DESTROY with bogus id returns not-found/inval",
                           "unexpected error code");
        }
    }

    /* CREATE — no kernel staged; expect UNIMPL or error */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_VM_CREATE, AOS_ERR_UNIMPL,
                         "linux_vmm: CREATE returns ok or unimpl");
}
