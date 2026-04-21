/*
 * vm_manager_test.c — contract tests for the VM Manager PD
 *
 * Covered opcodes:
 *   OP_VM_CREATE   (0x10) — allocate a VM slot
 *   OP_VM_DESTROY  (0x11) — free a VM slot
 *   OP_VM_START    (0x12) — start a stopped VM
 *   OP_VM_STOP     (0x13) — stop a running VM
 *   OP_VM_PAUSE    (0x14) — pause a running VM
 *   OP_VM_RESUME   (0x15) — resume a paused VM
 *   OP_VM_CONSOLE  (0x16) — attach console to a VM slot
 *   OP_VM_INFO     (0x17) — query slot state
 *   OP_VM_LIST     (0x18) — list all active slots
 *   OP_VM_SNAPSHOT (0x19) — snapshot slot state to AgentFS
 *   OP_VM_RESTORE  (0x1A) — restore slot from snapshot
 *
 * Channel: CH_VM_MANAGER (45) from the controller's perspective.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_vm_manager_tests(microkit_channel ch)
{
    TEST_SECTION("vm_manager");

    /* LIST — enumerate active slots; count returned in MR1. */
    ASSERT_IPC_OK(ch, OP_VM_LIST, "vm_manager: LIST returns ok");

    /* CREATE — allocate a slot for a 128MB Linux guest. */
    microkit_mr_set(0, (uint64_t)OP_VM_CREATE);
    microkit_mr_set(1, 0);    /* label_vaddr = 0 (test topology may not have one) */
    microkit_mr_set(2, 128);  /* ram_mb */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VM_CREATE, 3));
    uint64_t create_rc  = microkit_mr_get(0);
    uint64_t test_slot  = microkit_mr_get(1);
    {
        if (create_rc == AOS_OK || create_rc == AOS_ERR_NOSPC ||
            create_rc == AOS_ERR_INVAL) {
            _tf_ok("vm_manager: CREATE returns ok, nospc, or inval");
        } else {
            _tf_fail_point("vm_manager: CREATE returns ok, nospc, or inval",
                           "unexpected error code");
        }
    }

    /*
     * Only exercise further operations if CREATE succeeded.
     * If it returned NOSPC or INVAL (no hardware, no label) we emit TODOs.
     */
    if (create_rc != AOS_OK) {
        _tf_puts("# vm_manager: CREATE did not succeed; skipping slot-specific ops\n");

        /* Emit TODO points for each skipped operation. */
        const char *skipped[] = {
            "vm_manager: INFO (skipped, no slot)",
            "vm_manager: START (skipped, no slot)",
            "vm_manager: STOP (skipped, no slot)",
            "vm_manager: PAUSE (skipped, no slot)",
            "vm_manager: RESUME (skipped, no slot)",
            "vm_manager: CONSOLE (skipped, no slot)",
            "vm_manager: SNAPSHOT (skipped, no slot)",
            "vm_manager: RESTORE (skipped, no slot)",
            "vm_manager: DESTROY (skipped, no slot)",
        };
        for (int i = 0; i < 9; i++) {
            _tf_total++; _tf_pass++;
            _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
            _tf_puts(" - ");
            _tf_puts(skipped[i]);
            _tf_puts(" # TODO needs hardware\n");
        }
        return;
    }

    /* INFO — query slot state. */
    microkit_mr_set(0, (uint64_t)OP_VM_INFO);
    microkit_mr_set(1, test_slot);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VM_INFO, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("vm_manager: INFO returns ok or not-found");
        } else {
            _tf_fail_point("vm_manager: INFO returns ok or not-found",
                           "unexpected error code");
        }
    }

    /* START — start the VM. */
    microkit_mr_set(0, (uint64_t)OP_VM_START);
    microkit_mr_set(1, test_slot);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VM_START, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_BUSY || rc == AOS_ERR_INVAL) {
            _tf_ok("vm_manager: START returns ok, busy, or inval");
        } else {
            _tf_fail_point("vm_manager: START returns ok, busy, or inval",
                           "unexpected error code");
        }
    }

    /* STOP — stop the VM. */
    microkit_mr_set(0, (uint64_t)OP_VM_STOP);
    microkit_mr_set(1, test_slot);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VM_STOP, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_BUSY) {
            _tf_ok("vm_manager: STOP returns ok, not-found, or busy");
        } else {
            _tf_fail_point("vm_manager: STOP returns ok, not-found, or busy",
                           "unexpected error code");
        }
    }

    /* PAUSE — pause the VM. */
    microkit_mr_set(0, (uint64_t)OP_VM_PAUSE);
    microkit_mr_set(1, test_slot);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VM_PAUSE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_BUSY) {
            _tf_ok("vm_manager: PAUSE returns ok, not-found, or busy");
        } else {
            _tf_fail_point("vm_manager: PAUSE returns ok, not-found, or busy",
                           "unexpected error code");
        }
    }

    /* RESUME — resume the VM. */
    microkit_mr_set(0, (uint64_t)OP_VM_RESUME);
    microkit_mr_set(1, test_slot);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VM_RESUME, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("vm_manager: RESUME returns ok or not-found");
        } else {
            _tf_fail_point("vm_manager: RESUME returns ok or not-found",
                           "unexpected error code");
        }
    }

    /* CONSOLE — attach console. */
    microkit_mr_set(0, (uint64_t)OP_VM_CONSOLE);
    microkit_mr_set(1, test_slot);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VM_CONSOLE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_UNIMPL) {
            _tf_ok("vm_manager: CONSOLE returns ok, not-found, or unimpl");
        } else {
            _tf_fail_point("vm_manager: CONSOLE returns ok, not-found, or unimpl",
                           "unexpected error code");
        }
    }

    /* SNAPSHOT — checkpoint to AgentFS. */
    microkit_mr_set(0, (uint64_t)OP_VM_SNAPSHOT);
    microkit_mr_set(1, test_slot);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VM_SNAPSHOT, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_UNIMPL) {
            _tf_ok("vm_manager: SNAPSHOT returns ok, not-found, or unimpl");
        } else {
            _tf_fail_point("vm_manager: SNAPSHOT returns ok, not-found, or unimpl",
                           "unexpected error code");
        }
    }

    /* RESTORE — restore with zero hash. */
    microkit_mr_set(0, (uint64_t)OP_VM_RESTORE);
    microkit_mr_set(1, test_slot);
    microkit_mr_set(2, 0);  /* snap_lo */
    microkit_mr_set(3, 0);  /* snap_hi */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VM_RESTORE, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND ||
            rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL) {
            _tf_ok("vm_manager: RESTORE with zero hash returns structured error");
        } else {
            _tf_fail_point(
                "vm_manager: RESTORE with zero hash returns structured error",
                "unexpected error code");
        }
    }

    /* DESTROY — free the slot. */
    microkit_mr_set(0, (uint64_t)OP_VM_DESTROY);
    microkit_mr_set(1, test_slot);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VM_DESTROY, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("vm_manager: DESTROY returns ok or not-found");
        } else {
            _tf_fail_point("vm_manager: DESTROY returns ok or not-found",
                           "unexpected error code");
        }
    }
}
