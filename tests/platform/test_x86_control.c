#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <platform/x86_control.h>
#include "sel4_ipc.h"
#include "system_desc.h"
#include "contracts/guest_contract.h"
#include "contracts/serial_virt_contract.h"
#include "contracts/blk_virt_contract.h"
#include "contracts/net_virt_contract.h"
#ifdef AGENTOS_X86_USERSPACE_PROOF
#include "contracts/x86_vtx_proof.h"
bool aos_x86_lifecycle_ack;
#endif

static seL4_Word mrs[120], incoming_badge;
static seL4_MessageInfo_t incoming_info;
static sel4_msg_t received_reply;
static unsigned receives, polls, replies, suspends, resumes, teardowns, wakes;
static bool transition_ok = true, teardown_ok;
static uint32_t state = GUEST_STATE_RUNNING;
static bool started = true;
seL4_Word seL4_GetMR(int n) { assert(n >= 0 && n < 120); return mrs[n]; }
void seL4_SetMR(int n, seL4_Word v) { assert(n >= 0 && n < 120); mrs[n] = v; }
static seL4_MessageInfo_t receive(seL4_CPtr ep, seL4_Word *badge, seL4_CPtr reply)
{
    assert(ep == PD_CNODE_SLOT_SELF_EP && reply == AGENTOS_IPC_REPLY_CAP);
    *badge = incoming_badge;
    return incoming_info;
}
seL4_MessageInfo_t seL4_Recv(seL4_CPtr ep, seL4_Word *b, seL4_CPtr reply)
{ assert(state != GUEST_STATE_RUNNING); receives++; return receive(ep, b, reply); }
seL4_MessageInfo_t seL4_NBRecv(seL4_CPtr ep, seL4_Word *b, seL4_CPtr reply)
{ assert(state == GUEST_STATE_RUNNING); polls++; return receive(ep, b, reply); }
void seL4_Send(seL4_CPtr ep, seL4_MessageInfo_t info)
{
    assert(ep == AGENTOS_IPC_REPLY_CAP && info.length == _SEL4_MR_COUNT);
    _sel4_mrs_to_msg(&received_reply);
    assert(info.label == received_reply.opcode);
    replies++;
    /* A real IPC clobbers scratch MRs. No caller may rely on them later. */
    memset(mrs, 0xac, sizeof(mrs));
}
static bool suspend_guest(void) { suspends++; return transition_ok; }
static bool resume_guest(void) { resumes++; return transition_ok; }
static bool teardown_guest(void) { teardowns++; return teardown_ok; }
static void wake(seL4_Word badge, void *context)
{
    assert(context == &state);
    assert(badge == (SERIAL_VIRT_VMM_WAKE_BADGE | BLK_VIRT_VMM_WAKE_BADGE |
                     NET_VIRT_VMM_WAKE_BADGE));
    wakes++;
    memset(mrs, 0xdb, sizeof(mrs));
}
static const aos_guest_vmm_runtime_t runtime = {
    .os_type = 1u, .guest_id = 0u, .state = &state, .started = &started,
    .suspend = suspend_guest, .resume = resume_guest, .teardown = teardown_guest,
};
static void request(uint32_t op, uint32_t id)
{
    sel4_msg_t req = {.opcode = op, .length = 4};
    rep_u32(&req, 0, id);
    _sel4_msg_to_mrs(&req);
    incoming_badge = ((seL4_Word)SVC_ID_GUEST_VMM_PRIMARY << 48) | (UINT64_C(7) << 32);
    incoming_info = seL4_MessageInfo_new(op, 0, 0, _SEL4_MR_COUNT);
}
static void call(uint32_t op, uint32_t id, uint32_t status, uint32_t next)
{
    request(op, id);
    unsigned old = replies;
    enum aos_x86_control_result r = aos_x86_control_step(&runtime, wake, &state);
    assert(replies == old + 1 && received_reply.opcode == status && state == next);
    assert(r == (state == GUEST_STATE_RUNNING ? AOS_X86_CONTROL_RUNNING : AOS_X86_CONTROL_STOPPED));
}
int main(void)
{
    incoming_badge = 0;
    assert(aos_x86_control_step(&runtime, wake, &state) == AOS_X86_CONTROL_RUNNING);
    assert(polls == 1 && replies == 0);
#ifdef AGENTOS_X86_USERSPACE_PROOF
    call(AOS_X86_LIFECYCLE_ACK, AOS_X86_USERSPACE_PASS,
         GUEST_ERR_PROTOCOL_VIOLATION, GUEST_STATE_RUNNING);
    assert(!aos_x86_lifecycle_ack);
#endif
    call(MSG_GUEST_SUSPEND, 1, GUEST_ERR_BAD_GUEST_ID, GUEST_STATE_RUNNING);
    transition_ok = false;
    call(MSG_GUEST_SUSPEND, 0, GUEST_ERR_NOT_READY, GUEST_STATE_RUNNING);
    transition_ok = true;
    call(MSG_GUEST_SUSPEND, 0, GUEST_OK, GUEST_STATE_SUSPENDED);
    unsigned old = suspends;
    call(MSG_GUEST_SUSPEND, 0, GUEST_OK, GUEST_STATE_SUSPENDED);
    assert(suspends == old);
    call(MSG_GUEST_BOOT, 0, GUEST_ERR_BAD_STATE, GUEST_STATE_SUSPENDED);
    incoming_badge = SERIAL_VIRT_VMM_WAKE_BADGE | BLK_VIRT_VMM_WAKE_BADGE | NET_VIRT_VMM_WAKE_BADGE;
    old = replies;
    assert(aos_x86_control_step(&runtime, wake, &state) == AOS_X86_CONTROL_STOPPED);
    assert(wakes == 1 && replies == old);
    transition_ok = false;
    call(MSG_GUEST_RESUME, 0, GUEST_ERR_NOT_READY, GUEST_STATE_SUSPENDED);
    transition_ok = true;
    call(MSG_GUEST_RESUME, 0, GUEST_OK, GUEST_STATE_RUNNING);
    assert(resumes == 2);
    /* Malformed frames must not execute the otherwise valid DESTROY. */
    for (unsigned variant = 0; variant < 7; variant++) {
        request(MSG_GUEST_DESTROY, 0);
        switch (variant) {
        case 0: incoming_info.length--; break;
        case 1: incoming_info.length++; break;
        case 2: incoming_info.extra = 1; break;
        case 3: incoming_info.caps = 1; break;
        case 4: incoming_info.label++; break;
        case 5: mrs[0] |= UINT64_C(1) << 32; break;
        case 6: mrs[1] = UINT64_C(1) << 32; break;
        }
        assert(aos_x86_control_step(&runtime, wake, &state) == AOS_X86_CONTROL_RUNNING);
        assert(received_reply.opcode == GUEST_ERR_PROTOCOL_VIOLATION && teardowns == 0);
    }
    call(UINT32_MAX, 0, GUEST_ERR_PROTOCOL_VIOLATION, GUEST_STATE_RUNNING);
    call(MSG_GUEST_DESTROY, 0, GUEST_ERR_NOT_READY, GUEST_STATE_DESTROYING);
    call(MSG_GUEST_RESUME, 0, GUEST_ERR_BAD_STATE, GUEST_STATE_DESTROYING);
    call(MSG_GUEST_CREATE, 0, GUEST_ERR_BAD_STATE, GUEST_STATE_DESTROYING);
    teardown_ok = true;
    call(MSG_GUEST_DESTROY, 0, GUEST_OK, GUEST_STATE_DEAD);
    assert(teardowns == 2 && !started);
    call(MSG_GUEST_DESTROY, 0, GUEST_OK, GUEST_STATE_DEAD);
    assert(teardowns == 2);
    call(MSG_GUEST_RESUME, 0, GUEST_ERR_DEAD, GUEST_STATE_DEAD);
    call(MSG_GUEST_CREATE, 0, GUEST_ERR_DEAD, GUEST_STATE_DEAD);
#ifdef AGENTOS_X86_USERSPACE_PROOF
    request(AOS_X86_LIFECYCLE_ACK, AOS_X86_USERSPACE_PASS);
    incoming_badge = ((seL4_Word)SVC_ID_GUEST_VMM_PRIMARY << 48) | (UINT64_C(6) << 32);
    assert(aos_x86_control_step(&runtime, wake, &state) == AOS_X86_CONTROL_STOPPED);
    assert(!aos_x86_lifecycle_ack && received_reply.opcode == GUEST_ERR_PROTOCOL_VIOLATION);
    call(AOS_X86_LIFECYCLE_ACK, AOS_X86_USERSPACE_PASS, GUEST_OK, GUEST_STATE_DEAD);
    assert(aos_x86_lifecycle_ack);
#endif
    incoming_badge = SERIAL_VIRT_VMM_WAKE_BADGE | 1u;
    assert(aos_x86_control_step(&runtime, wake, &state) == AOS_X86_CONTROL_ERROR);
    assert(wakes == 1 && receives > 0 && polls > 0);
    puts("PASS: x86 control framing, notification dispatch, suspend/resume and terminal retries");
}
