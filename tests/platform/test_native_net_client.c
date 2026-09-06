#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <contracts/net-service/interface.h>
#include "native_net_client.h"

static uint8_t shared[NET_SVC_SHMEM_TOTAL];
static uint32_t last_opcode;
static uint32_t last_arg0;
static uint32_t last_arg1;

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
    uint32_t v;
    memcpy(&v, p + off, sizeof(v));
    return v;
}

static void wr32(uint8_t *p, uint32_t off, uint32_t value)
{
    memcpy(p + off, &value, sizeof(value));
}

static void fake_call(uintptr_t endpoint, const sel4_msg_t *req,
                      sel4_msg_t *rep)
{
    (void)endpoint;
    *rep = (sel4_msg_t){0};
    last_opcode = req->opcode;
    last_arg0 = rd32(req->data, 0u);
    last_arg1 = rd32(req->data, 4u);
    wr32(rep->data, 0u, NET_SVC_RAW_OK);
    rep->opcode = 0u;
    rep->length = 12u;

    switch (req->opcode) {
    case NET_SVC_OP_RAW_OPEN:
        wr32(rep->data, 4u, 7u);
        wr32(rep->data, 8u, NET_SVC_SLOT_BASE + 2u * NET_SVC_SLOT_SIZE);
        break;
    case NET_SVC_OP_RAW_STATUS:
        wr32(rep->data, 4u, 1u);
        break;
    case NET_SVC_OP_RAW_RECV:
        wr32(rep->data, 4u, 4u);
        wr32(rep->data, 8u, NET_SVC_SLOT_BASE + 2u * NET_SVC_SLOT_SIZE +
                              NET_SVC_HDR_SIZE);
        memcpy(shared + rd32(rep->data, 8u), "pong", 4u);
        break;
    default:
        break;
    }
}

static int check(int condition, const char *name)
{
    printf("%s - %s\n", condition ? "ok" : "not ok", name);
    return condition ? 0 : 1;
}

int main(void)
{
    int failed = 0;
    bool link_up = false;
    char frame[8] = {0};
    native_net_client_t client;

    failed += check(native_net_client_open(
                        &client, 12u, shared, sizeof(shared), 2u, fake_call) &&
                    client.ready && client.handle == 7u &&
                    last_opcode == NET_SVC_OP_RAW_OPEN && last_arg0 == 2u,
                    "native agent gets an isolated net-service slot");
    failed += check(native_net_client_status(&client, &link_up) && link_up &&
                    last_opcode == NET_SVC_OP_RAW_STATUS && last_arg0 == 7u,
                    "native agent observes host link state");
    failed += check(native_net_client_send(&client, "ping", 4u) &&
                    last_opcode == NET_SVC_OP_RAW_SEND &&
                    last_arg0 == 7u && last_arg1 == 4u &&
                    memcmp(shared + client.slot_offset + NET_SVC_HDR_SIZE,
                           "ping", 4u) == 0,
                    "native agent sends through its shared slot");
    failed += check(native_net_client_recv(&client, frame, sizeof(frame)) == 4 &&
                    memcmp(frame, "pong", 4u) == 0 &&
                    last_opcode == NET_SVC_OP_RAW_RECV,
                    "native agent receives through its shared slot");

    printf("1..4\n");
    return failed == 0 ? 0 : 1;
}
