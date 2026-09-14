#include <platform/net_server_loop.h>
#include <setjmp.h>
#include <stdio.h>

static jmp_buf done;
static seL4_Word input_badge, input_label, request_badge, reply_label;
static unsigned receives, irqs, requests, replies;

seL4_MessageInfo_t seL4_Recv(seL4_CPtr ep, seL4_Word *badge, seL4_CPtr reply)
{
    (void)ep; (void)reply;
    if (receives++) longjmp(done, 1);
    *badge = input_badge;
    return (seL4_MessageInfo_t){input_label};
}
void seL4_Send(seL4_CPtr ep, seL4_MessageInfo_t info)
{
    (void)ep; replies++; reply_label = info.label;
}
static void irq(void) { irqs++; }
static seL4_MessageInfo_t request(seL4_Word badge)
{
    requests++; request_badge = badge;
    return (seL4_MessageInfo_t){123};
}
static int run(seL4_Word badge, seL4_Word label, int expect_irq)
{
    input_badge = badge; input_label = label;
    receives = irqs = requests = replies = 0;
    if (!setjmp(done)) aos_net_server_loop(1, 9, irq, request);
    return expect_irq ? irqs == 1 && !requests && !replies :
        !irqs && requests == 1 && replies == 1 &&
        request_badge == badge && reply_label == 123;
}
int main(void)
{
    unsigned checks = 0, failures = 0;
    const seL4_Word labels[] = {0, 1, 0x2100, 0x3ffffdf};
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        int ok = run(AOS_NET_HOST_IRQ_BADGE, labels[i], 1);
        printf("%s %u - IRQ acknowledged without dispatching or replying to stale IPC\n",
               ok ? "ok" : "not ok", ++checks);
        failures += !ok;
    }
    const seL4_Word badges[] = {0, 17, 27, 0xa0510001};
    for (unsigned i = 0; i < sizeof(badges) / sizeof(badges[0]); i++) {
        int ok = run(badges[i], 0, 0);
        printf("%s %u - RPC badge and reply preserved even with zero label\n",
               ok ? "ok" : "not ok", ++checks);
        failures += !ok;
    }
    printf("1..%u\n", checks);
    return failures != 0;
}
