#include <assert.h>
#include <string.h>
#include "../api/framework.h"
#include "sel4_server.h"

static void identity_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep);
#define sel4_call identity_call
#include "../../services/vm-manager/vm_manager.c"
#undef sel4_call

static uint32_t returned_id[2] = {17u, 91u};
static unsigned calls;
static bool malformed_create;
static sel4_msg_t last_request;
static seL4_CPtr last_endpoint;

static void identity_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    assert(ep == 100u || ep == 101u);
    unsigned owner = ep == 101u;
    calls++;
    last_request = *req;
    last_endpoint = ep;
    memset(rep, 0, sizeof(*rep));
    rep->opcode = GUEST_OK;
    if (req->opcode == MSG_GUEST_CREATE) {
        assert(msg_u32(req, 0u) == owner + 1u);
        rep_u32(rep, 0u, GUEST_OK);
        rep_u32(rep, 4u, returned_id[owner]);
        rep->length = malformed_create ? 4u : 8u;
    } else {
        assert(req->length >= 4u);
        assert(msg_u32(req, 0u) == returned_id[owner]);
        if (req->opcode == MSG_GUEST_CONSOLE_DRAIN) {
            rep->data[0] = 'o'; rep->data[1] = 'k'; rep->length = 2u;
        }
    }
}

int main(void)
{
    g_primary_vmm_ep = 100u;
    g_secondary_vmm_ep = 101u;
    uint8_t primary, secondary;
    assert(dedicated_create(VM_PROFILE_PRIMARY, 64u, 0u, &primary) == 0);
    assert(primary == 0u && last_request.opcode == MSG_GUEST_BOOT);
    assert(msg_u32(&last_request, 0u) == 17u);
    assert(dedicated_create(VM_PROFILE_SECONDARY, 64u, 0u, &secondary) == 0);
    assert(secondary == 1u && last_request.opcode == MSG_GUEST_BOOT);
    assert(msg_u32(&last_request, 0u) == 91u);
    assert(dedicated_guest_call(secondary, MSG_GUEST_SUSPEND, VM_SLOT_SUSPENDED) == VM_OK);
    assert(last_endpoint == 101u);
    assert(dedicated_guest_call(primary, MSG_GUEST_RESUME, VM_SLOT_RUNNING) == VM_OK);
    assert(last_endpoint == 100u);

    sel4_msg_t request = {.length = 28u}, reply = {0};
    rep_u32(&request, 0u, secondary);
    request.data[7] = 0xa5;
    assert(h_send_input(0, &request, &reply, NULL) == SEL4_ERR_OK);
    assert(last_request.opcode == MSG_GUEST_SEND_INPUT && last_request.data[7] == 0xa5);
    request.length = 8u;
    rep_u32(&request, 4u, 8u);
    assert(h_console_drain(0, &request, &reply, NULL) == SEL4_ERR_OK);
    assert(msg_u32(&last_request, 4u) == 8u);
    assert(reply.length == 10u && memcmp(reply.data + 8u, "ok", 2u) == 0);
    assert(dedicated_destroy(secondary) == VM_OK);
    unsigned before = calls;
    assert(dedicated_guest_call(secondary, MSG_GUEST_BOOT, VM_SLOT_RUNNING) == VM_ERR);
    assert(calls == before && g_slot_guest_bound[primary]);

    malformed_create = true;
    assert(dedicated_create(VM_PROFILE_SECONDARY, 64u, 0u, &secondary) == -3);
    assert(calls == before + 1u && !g_slot_guest_bound[1]);
    malformed_create = false;
    returned_id[1] = 123u;
    assert(dedicated_create(VM_PROFILE_SECONDARY, 64u, 0u, &secondary) == 0);
    assert(msg_u32(&last_request, 0u) == 123u);
    assert(g_slot_guest_id[primary] == 17u);
    puts("PASS: VM manager binds CREATE identities across control, input, drain and recreation");
    return 0;
}
