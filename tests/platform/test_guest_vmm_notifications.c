#include <platform/guest_vmm_loop.h>
#include <platform/input.h>
#include <contracts/serial_virt_contract.h>
#include <contracts/blk_virt_contract.h>
#include <contracts/guest_contract.h>
#include <setjmp.h>
#include <stdio.h>

static jmp_buf complete;
static seL4_Word incoming_badge, incoming_label, observed_badge;
static unsigned receives, notifications, rpcs, faults, sends, checks, failures;
seL4_MessageInfo_t seL4_Recv(seL4_CPtr endpoint, seL4_Word *badge, seL4_CPtr reply)
{
    (void)endpoint; (void)reply;
    if (receives++) longjmp(complete, 1);
    *badge = incoming_badge;
    return (seL4_MessageInfo_t){incoming_label};
}
void seL4_Send(seL4_CPtr destination, seL4_MessageInfo_t info)
{
    (void)destination; (void)info; sends++;
}
static void notified(seL4_Word badge) { notifications++; observed_badge = badge; }
static seL4_MessageInfo_t rpc(seL4_MessageInfo_t info) { rpcs++; return info; }
static seL4_MessageInfo_t fault(seL4_Word badge, seL4_MessageInfo_t info)
{
    faults++; observed_badge = badge; return info;
}
static void ready(void) {}
static void check(int condition, const char *name)
{
    printf("%s %u - %s\n", condition ? "ok" : "not ok", ++checks, name);
    failures += !condition;
}
static void dispatch(seL4_Word badge, seL4_Word label)
{
    incoming_badge = badge; incoming_label = label;
    receives = notifications = rpcs = faults = sends = 0;
    uint32_t state = GUEST_STATE_RUNNING;
    const aos_guest_vmm_loop_ops_t ops = {&state, rpc, fault, notified, ready, ready};
    if (!setjmp(complete)) aos_guest_vmm_loop(1, 9, &ops);
}
int main(void)
{
    dispatch(SERIAL_VIRT_VMM_WAKE_BADGE, 0x3ffffdf);
    check(notifications == 1 && !rpcs && !faults && !sends &&
          observed_badge == SERIAL_VIRT_VMM_WAKE_BADGE,
          "nonzero stale notification label never becomes a guest fault or reply");
    dispatch(SERIAL_VIRT_VMM_WAKE_BADGE, MSG_GUEST_CREATE);
    check(notifications == 1 && !rpcs && !faults && !sends,
          "notification cannot impersonate an RPC using a stale label");
    dispatch(0, MSG_GUEST_CREATE);
    check(rpcs == 1 && sends == 1 && !notifications && !faults,
          "real lifecycle IPC retains its normal reply path");
    dispatch(AOS_INPUT_VMM_WAKE_BADGE, MSG_GUEST_DESTROY);
    check(notifications == 1 && !rpcs && !faults && !sends &&
          observed_badge == AOS_INPUT_VMM_WAKE_BADGE,
          "input wake cannot execute a stale lifecycle RPC");
    dispatch(AOS_INPUT_VMM_WAKE_BADGE | SERIAL_VIRT_VMM_WAKE_BADGE, 7);
    check(notifications == 1 && !rpcs && !faults && !sends &&
          observed_badge == (AOS_INPUT_VMM_WAKE_BADGE | SERIAL_VIRT_VMM_WAKE_BADGE),
          "coalesced input and serial notifications retain both bits");
    dispatch(BLK_VIRT_VMM_WAKE_BADGE, MSG_GUEST_DESTROY);
    check(notifications == 1 && !rpcs && !faults && !sends &&
          observed_badge == BLK_VIRT_VMM_WAKE_BADGE,
          "block wake cannot execute stale destroy RPC");
    dispatch(BLK_VIRT_VMM_WAKE_BADGE | SERIAL_VIRT_VMM_WAKE_BADGE, 7);
    check(notifications == 1 && !rpcs && !faults && !sends &&
          observed_badge == (BLK_VIRT_VMM_WAKE_BADGE | SERIAL_VIRT_VMM_WAKE_BADGE),
          "coalesced serial and block bits both reach notification handler");
    check(blk_virt_service_notification(1) && blk_virt_service_notification(2) &&
          blk_virt_service_notification(3) && !blk_virt_service_notification(0) &&
          !blk_virt_service_notification(virt_client_badge(0)) &&
          !blk_virt_service_notification(virt_client_badge(1)),
          "block client wake bits cannot alias attachment authority");
    dispatch(UINT64_C(1) << 62, 7);
    check(faults == 1 && sends == 1 && !notifications && !rpcs,
          "real VCPU fault badge remains on the fault path");
    for (uint64_t badge = 1; badge <= 7; badge++)
        check(serial_virt_service_notification(badge),
              "combined serial notification bits classify without an IPC label");
    check(!serial_virt_service_notification(0) &&
          !serial_virt_service_notification(SERIAL_VIRT_FRONTEND_BADGE) &&
          !serial_virt_service_notification(virt_client_badge(0)) &&
          !serial_virt_service_notification(virt_client_badge(1)),
          "attachment badges cannot be mistaken for notifications");
    printf("1..%u\n", checks);
    return failures != 0;
}
