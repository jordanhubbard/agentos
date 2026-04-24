/*
 * agentOS — vmm PD contract test
 * Covered opcodes:
 *   Section A (external): MSG_VM_CREATE, MSG_VM_DESTROY, MSG_VM_SWITCH,
 *     MSG_VM_STATUS, MSG_VM_LIST
 *   Section B (VMM-kernel): MSG_VMM_REGISTER, MSG_VMM_ALLOC_GUEST_MEM,
 *     MSG_VMM_VCPU_CREATE, MSG_VMM_VCPU_DESTROY, MSG_VMM_VCPU_SET_REGS,
 *     MSG_VMM_VCPU_GET_REGS, MSG_VMM_INJECT_IRQ
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/vmm_contract.h"

void run_vmm_tests(microkit_channel ch) {
    TEST_SECTION("vmm");

    /* --- Section A: External VM management API --- */

    /* LIST — should always succeed */
    ASSERT_IPC_OK(ch, MSG_VM_LIST, "vmm list");

    /* STATUS — bogus vm id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VM_STATUS, VMM_ERR_NOT_FOUND, "vmm status bogus id");

    /* CREATE — no setup, expect INVAL */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_VM_CREATE, VMM_ERR_INVAL, "vmm create no args");

    /* DESTROY — bogus vm id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VM_DESTROY, VMM_ERR_NOT_FOUND, "vmm destroy bogus id");

    /* SWITCH — bogus vm id */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VM_SWITCH, VMM_ERR_NOT_FOUND, "vmm switch bogus id");

    /* --- Section B: VMM-to-root-task internal API --- */

    /* REGISTER — expect OK or already registered */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_VMM_REGISTER, VMM_ERR_EXISTS, "vmm register");

    /* ALLOC_GUEST_MEM — bogus size */
    seL4_SetMR(1, 0);
    ASSERT_IPC_OK_OR_ERR(ch, MSG_VMM_ALLOC_GUEST_MEM, VMM_ERR_INVAL, "vmm alloc guest mem zero");

    /* VCPU_CREATE — bogus vm */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VMM_VCPU_CREATE, VMM_ERR_NOT_FOUND, "vmm vcpu create bogus");

    /* VCPU_DESTROY — bogus vcpu */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VMM_VCPU_DESTROY, VMM_ERR_NOT_FOUND, "vmm vcpu destroy bogus");

    /* VCPU_SET_REGS — bogus vcpu */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VMM_VCPU_SET_REGS, VMM_ERR_NOT_FOUND, "vmm vcpu set regs bogus");

    /* VCPU_GET_REGS — bogus vcpu */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VMM_VCPU_GET_REGS, VMM_ERR_NOT_FOUND, "vmm vcpu get regs bogus");

    /* INJECT_IRQ — bogus vm */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_VMM_INJECT_IRQ, VMM_ERR_NOT_FOUND, "vmm inject irq bogus");

    /* Verify constants */
    ASSERT_TRUE(VMM_MAX_VCPUS == 4, "vmm max vcpus == 4");
    ASSERT_TRUE(VMM_OK == 0, "vmm ok == 0");
}
