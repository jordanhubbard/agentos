/* Ordinary seL4 client: no device, guest-memory or execution capabilities. */
#include "sel4_ipc.h"
#include "system_desc.h"
#include "contracts/guest_contract.h"
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
void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)endpoint; (void)nameserver;
    phase(AOS_X86_LIFECYCLE_READY);
    expect(MSG_GUEST_SUSPEND, 1u, GUEST_ERR_BAD_GUEST_ID);
    expect(MSG_GUEST_SUSPEND, 0u, GUEST_OK);
    expect(MSG_GUEST_SUSPEND, 0u, GUEST_OK);
    expect(MSG_GUEST_BOOT, 0u, GUEST_ERR_BAD_STATE);
    expect(MSG_GUEST_CREATE, 0u, GUEST_ERR_BAD_STATE);
    expect(MSG_GUEST_RESUME, 0u, GUEST_OK);
    expect(AOS_X86_LIFECYCLE_BOOT_ACK, AOS_X86_USERSPACE_PASS, GUEST_OK);
    phase(AOS_X86_LIFECYCLE_CHECKPOINT);
    expect(MSG_GUEST_SUSPEND, 0u, GUEST_OK);
    bool done = false;
    for (unsigned i = 0; i < 100000u; i++) {
        uint32_t status = call(MSG_GUEST_DESTROY, 0u);
        if (status == GUEST_OK) { done = true; break; }
        if (status != GUEST_ERR_NOT_READY) fail();
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
