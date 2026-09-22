/* Ordinary seL4 client: no device, guest-memory or execution capabilities. */
#include "sel4_ipc.h"
#include "system_desc.h"
#include "contracts/guest_contract.h"
#include "contracts/vm_manager_contract.h"
#include "contracts/x86_vtx_proof.h"

static seL4_Word last_operation, last_status, expected_status;
#ifdef AGENTOS_X86_LIFECYCLE_WITNESS
static volatile aos_x86_lifecycle_witness_t probe_witness = {
    .magic = UINT64_C(0x414f534c50524f42), .version = UINT64_C(0x4c49464557495431),
};
#define PROBE_STAGE(n, op) do { probe_witness.stage = (n); probe_witness.opcode = (op); } while (0)
#else
#define PROBE_STAGE(n, op) ((void)0)
#endif
static _Noreturn void fail(void)
{
    PROBE_STAGE(5, last_operation);
    seL4_SetMR(0, AOS_X86_VTX_PROOF_FAIL);
    seL4_SetMR(1, last_operation);
    seL4_SetMR(2, last_status);
    seL4_SetMR(3, expected_status);
    seL4_Send(AOS_X86_VTX_REPORT_CAP,
        seL4_MessageInfo_new(AOS_X86_VTX_PROOF_LABEL, 0u, 0u, 4u));
    for (;;) seL4_Yield();
}
static void phase(uint32_t expected)
{
    PROBE_STAGE(1, expected);
    seL4_Word badge;
    seL4_MessageInfo_t info = seL4_Recv(PD_CNODE_SLOT_SELF_EP, &badge, AGENTOS_IPC_REPLY_CAP);
    PROBE_STAGE(2, expected);
    last_operation = expected;
    last_status = badge;
    expected_status = ((seL4_Word)SVC_ID_X86_LIFECYCLE_PROBE << 48) |
                      ((seL4_Word)AOS_X86_LIFECYCLE_VMM_INDEX << 32);
    if (badge != (((seL4_Word)SVC_ID_X86_LIFECYCLE_PROBE << 48) |
                  ((seL4_Word)AOS_X86_LIFECYCLE_VMM_INDEX << 32)) ||
        seL4_MessageInfo_get_label(info) != expected ||
        seL4_MessageInfo_get_length(info) != 0u) fail();
}
static uint32_t call(uint32_t opcode, uint32_t value)
{
    sel4_msg_t req = {.opcode = opcode, .length = 4u}, rep = {0};
    rep_u32(&req, 0u, value);
    PROBE_STAGE(3, opcode);
    sel4_call(PD_CNODE_SLOT_GUEST_VMM_PRIMARY_EP, &req, &rep);
    PROBE_STAGE(4, opcode);
#ifdef AGENTOS_X86_LIFECYCLE_WITNESS
    probe_witness.status = rep.opcode;
    probe_witness.count++;
#endif
    last_operation = opcode;
    last_status = rep.opcode;
    return rep.opcode;
}
static void expect(uint32_t opcode, uint32_t value, uint32_t status)
{
    expected_status = status;
    if (call(opcode, value) != status) fail();
}
static sel4_msg_t manager(uint32_t opcode, uint32_t ram_mb)
{
    sel4_msg_t req = {.opcode = opcode, .length = 4u}, rep = {0};
    if (opcode == VM_MANAGER_OP_CREATE) {
        req.length = 12u;
        rep_u32(&req, 0u, VM_PROFILE_PRIMARY);
        rep_u32(&req, 4u, ram_mb);
    }
    PROBE_STAGE(3, opcode);
    sel4_call(PD_CNODE_SLOT_VM_MANAGER_EP, &req, &rep);
    PROBE_STAGE(4, opcode);
    last_operation = opcode;
    last_status = rep.opcode;
    return rep;
}
static void manager_ok(uint32_t opcode)
{
    sel4_msg_t rep = manager(opcode, 0u);
    expected_status = 0u;
    if (rep.opcode != SEL4_ERR_OK || rep.length < 4u || msg_u32(&rep, 0u) != 0u) fail();
}
static void manager_info(uint32_t state)
{
    sel4_msg_t rep = manager(VM_MANAGER_OP_INFO, 0u);
    expected_status = state;
    last_status = msg_u32(&rep, 4u);
    if (rep.opcode != SEL4_ERR_OK || rep.length != 48u ||
        msg_u32(&rep, 0u) != 0u || msg_u32(&rep, 4u) != state ||
        msg_u32(&rep, 8u) != VM_PROFILE_PRIMARY ||
        msg_u32(&rep, 12u) != (AOS_X86_FIRMWARE_RAM >> 20) ||
        msg_u32(&rep, 16u) != 1u ||
        msg_u32(&rep, 28u) != (uint32_t)AOS_X86_FIRMWARE_RAM_VA ||
        msg_u32(&rep, 32u) != 0u ||
        msg_u32(&rep, 36u) != 0u || msg_u32(&rep, 40u) != 0u) fail();
}
void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)endpoint; (void)nameserver;
    phase(AOS_X86_LIFECYCLE_READY);
    expect(MSG_GUEST_SUSPEND, 1u, GUEST_ERR_BAD_GUEST_ID);
    expect(MSG_GUEST_SUSPEND, 0u, GUEST_ERR_BAD_STATE);
    expect(MSG_GUEST_RESUME, 0u, GUEST_ERR_BAD_STATE);
    sel4_msg_t rejected = manager(VM_MANAGER_OP_CREATE,
                                  (AOS_X86_FIRMWARE_RAM >> 20) + 4u);
    expected_status = SEL4_ERR_BAD_ARG;
    if (rejected.opcode != SEL4_ERR_BAD_ARG || rejected.length != 4u ||
        msg_u32(&rejected, 0u) != 1u) fail();
    sel4_msg_t created = manager(VM_MANAGER_OP_CREATE, 0u);
    expected_status = SEL4_ERR_OK;
    if (created.opcode != SEL4_ERR_OK || created.length != 8u ||
        msg_u32(&created, 0u) != 0u || msg_u32(&created, 4u) != 0u) fail();
    manager_info(VM_WIRE_SLOT_RUNNING);
    manager_ok(VM_MANAGER_OP_PAUSE);
    manager_ok(VM_MANAGER_OP_PAUSE);
    manager_info(VM_WIRE_SLOT_SUSPENDED);
    expect(MSG_GUEST_BOOT, 0u, GUEST_ERR_BAD_STATE);
    expect(MSG_GUEST_CREATE, 0u, GUEST_ERR_BAD_STATE);
    manager_ok(VM_MANAGER_OP_RESUME);
    expect(AOS_X86_LIFECYCLE_BOOT_ACK, AOS_X86_USERSPACE_PASS, GUEST_OK);
    phase(AOS_X86_LIFECYCLE_CHECKPOINT);
    manager_ok(VM_MANAGER_OP_PAUSE);
    bool done = false;
    for (unsigned i = 0; i < 100000u; i++) {
        sel4_msg_t rep = manager(VM_MANAGER_OP_DESTROY, 0u);
        if (rep.opcode != SEL4_ERR_OK || rep.length != 4u) fail();
        if (msg_u32(&rep, 0u) == 0u) { done = true; break; }
        if (msg_u32(&rep, 0u) != 1u) fail();
        expect(MSG_GUEST_RESUME, 0u, GUEST_ERR_BAD_STATE);
        seL4_Yield();
    }
    if (!done) fail();
    expect(MSG_GUEST_DESTROY, 0u, GUEST_OK);
    expect(MSG_GUEST_RESUME, 0u, GUEST_ERR_DEAD);
    expect(MSG_GUEST_CREATE, 0u, GUEST_ERR_DEAD);
    expect(AOS_X86_LIFECYCLE_ACK, AOS_X86_USERSPACE_PASS, GUEST_OK);
    sel4_dbg_puts("[x86-lifecycle] client IPC sequence passed\n");
    for (;;) {
        seL4_Word badge;
        (void)seL4_Recv(PD_CNODE_SLOT_SELF_EP, &badge, AGENTOS_IPC_REPLY_CAP);
    }
}
