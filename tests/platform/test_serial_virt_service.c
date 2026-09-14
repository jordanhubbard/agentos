#include <platform/serial_virt_service.h>
#include <stdio.h>
#include <string.h>

_Alignas(8) static uint8_t pages[4][AOS_SERIAL_FRONTEND_STRIDE];
static unsigned checks, failures;
static void check(int result, const char *name)
{
    printf("%s %u - %s\n", result ? "ok" : "not ok", ++checks, name);
    failures += !result;
}

int main(void)
{
    for (uint32_t configured = 0; configured < 8; configured++) {
        for (uint32_t slot = 0; slot < 4; slot++) {
            uint32_t client = 99;
            int valid = aos_serial_client_for_backend(slot, configured, &client);
            int expected = (configured == 1 || configured == 2) ? slot == 0 :
                           configured == 3 && slot < 2;
            check(valid == expected && (!valid ? client == 99 :
                  client == (configured == 2 ? 1u : slot)),
                  "backend slots resolve only to configured serial pages");
        }
    }
    check(!aos_serial_client_for_backend(0, 1, NULL), "null client output rejected");
    aos_serial_virt_service_t service = {0};
    for (unsigned i = 0; i < AOS_SERIAL_CLIENTS; i++) {
        service.guest[i] = aos_serial_channel_at((uintptr_t)pages[i]);
        service.frontend[i] = aos_serial_channel_at((uintptr_t)pages[i + 2]);
        memcpy(service.guest[i].from_guest.data, i ? "XYZ" : "abc", 3);
        service.guest[i].from_guest.queue->tail = 3;
        memcpy(service.frontend[i].to_guest.data, i ? "12" : "34", 2);
        service.frontend[i].to_guest.queue->tail = 2;
    }
    serial_virt_attach_req_t req = {SERIAL_VIRT_CONTRACT_VERSION, 1, SERIAL_VIRT_ROLE_VMM};
    check(aos_serial_virt_attach(&service, VIRT_CLIENT_BADGE_PRIMARY, &req, sizeof(req)) ==
          SERIAL_VIRT_ERR_AUTHORITY && !service.guest_attached[1],
          "foreign attach cannot change service state");
    check(aos_serial_virt_attach(&service, VIRT_CLIENT_BADGE_SECONDARY, &req, sizeof(req) - 1) ==
          SERIAL_VIRT_ERR_PROTOCOL && !service.guest_attached[1],
          "short attach is rejected before registration");
    req.version++;
    check(aos_serial_virt_attach(&service, VIRT_CLIENT_BADGE_SECONDARY, &req, sizeof(req)) ==
          SERIAL_VIRT_ERR_VERSION && !service.guest_attached[1], "version mismatch fails closed");
    req.version = SERIAL_VIRT_CONTRACT_VERSION;
    check(aos_serial_virt_service_pump(&service, 8).bytes == 0 &&
          service.guest[0].from_guest.queue->head == 0, "unattached queues remain untouched");
    for (unsigned i = 0; i < AOS_SERIAL_CLIENTS; i++) {
        req.client = i; req.role = SERIAL_VIRT_ROLE_VMM;
        check(aos_serial_virt_attach(&service, virt_client_badge(i), &req, sizeof(req)) ==
              SERIAL_VIRT_OK && service.guest[i].from_guest.queue->tail == 3,
              "guest attach preserves already queued output");
        req.role = SERIAL_VIRT_ROLE_FRONTEND;
        check(aos_serial_virt_attach(&service, SERIAL_VIRT_FRONTEND_BADGE, &req, sizeof(req)) ==
              SERIAL_VIRT_OK, "frontend attaches to its authorized channel");
    }
    check(aos_serial_virt_attach(&service, SERIAL_VIRT_FRONTEND_BADGE, &req, sizeof(req)) ==
          SERIAL_VIRT_ERR_BUSY && service.frontend[1].to_guest.queue->tail == 2,
          "duplicate attach cannot reset queues");
    service.guest[0].meta->guest_state = 5;
    service.guest[1].meta->guest_state = 4;
    aos_serial_virt_result_t result = aos_serial_virt_service_pump(&service, 2);
    check(result.bytes == 8 && result.wake_vmm == 3 && !result.invalid_clients &&
          result.input_clients == 3 && result.output_clients == 3 &&
          !memcmp(service.frontend[0].from_guest.data, "ab", 2) &&
          !memcmp(service.frontend[1].from_guest.data, "XY", 2) &&
          !memcmp(service.guest[0].to_guest.data, "34", 2) &&
          !memcmp(service.guest[1].to_guest.data, "12", 2),
          "both directions route only to the matching client");
    check(service.frontend[0].meta->guest_state == 5 &&
          service.frontend[1].meta->guest_state == 4 &&
          service.frontend[0].meta->attached && service.frontend[1].meta->attached,
          "guest state is mirrored to the matching frontend");
    service.guest[0].from_guest.queue->tail = AOS_SERIAL_TX_CAPACITY + 3;
    result = aos_serial_virt_service_pump(&service, 8);
    check(result.invalid_clients == 1 && result.bytes == 1 && result.wake_vmm == 2 &&
          result.input_clients == 0 && result.output_clients == 2 &&
          service.guest[0].from_guest.queue->head == 2 &&
          !memcmp(service.frontend[1].from_guest.data, "XYZ", 3),
          "malformed client zero does not block client one");
    printf("1..%u\n", checks);
    return failures ? 1 : 0;
}
