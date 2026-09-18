#define _GNU_SOURCE
#include <platform/serial_virt_service.h>
#include <platform/serial_frontend.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

_Alignas(8) static uint8_t pages[2 * AOS_SERIAL_CLIENTS][AOS_SERIAL_FRONTEND_STRIDE];
static unsigned checks, failures;
static void check(int result, const char *name)
{
    printf("%s %u - %s\n", result ? "ok" : "not ok", ++checks, name);
    failures += !result;
}

int main(void)
{
    void *retired_page = mmap(NULL, AOS_SERIAL_FRAME_SIZE,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (retired_page == MAP_FAILED) return 1;
    aos_serial_virt_service_t service = {0};
    for (unsigned i = 0; i < AOS_SERIAL_CLIENTS; i++) {
        service.guest[i] = aos_serial_channel_at((uintptr_t)pages[i]);
        if (i == 0) service.guest[i] = aos_serial_channel_at((uintptr_t)retired_page);
        service.frontend[i] = aos_serial_channel_at((uintptr_t)pages[i + AOS_SERIAL_CLIENTS]);
        if (i >= 2) continue;
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
    for (unsigned i = 0; i < 2; i++) {
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
    req.client = 0; req.role = SERIAL_VIRT_ROLE_OPERATOR;
    check(aos_serial_virt_attach(&service, SERIAL_VIRT_OPERATOR_BADGE, &req, sizeof(req)) ==
          SERIAL_VIRT_ERR_AUTHORITY, "operator authority cannot attach a guest page");
    req.client = SERIAL_VIRT_OPERATOR_CLIENT; req.role = SERIAL_VIRT_ROLE_VMM;
    check(aos_serial_virt_attach(&service, VIRT_CLIENT_BADGE_PRIMARY, &req, sizeof(req)) ==
          SERIAL_VIRT_ERR_AUTHORITY, "guest authority cannot attach the operator page");
    req.role = SERIAL_VIRT_ROLE_OPERATOR;
    check(aos_serial_virt_attach(&service, SERIAL_VIRT_OPERATOR_BADGE, &req, sizeof(req)) ==
          SERIAL_VIRT_OK, "operator attaches only its native client page");
    req.role = SERIAL_VIRT_ROLE_FRONTEND;
    check(aos_serial_virt_attach(&service, SERIAL_VIRT_FRONTEND_BADGE, &req, sizeof(req)) ==
          SERIAL_VIRT_OK, "frontend attaches the distinct operator channel");
    memcpy(service.guest[2].from_guest.data, "reply", 5);
    service.guest[2].from_guest.queue->tail = 5;
    memcpy(service.frontend[2].to_guest.data, "insp", 4);
    service.frontend[2].to_guest.queue->tail = 4;
    result = aos_serial_virt_service_pump(&service, 8);
    check(result.bytes == 9 && result.wake_vmm == 4 && result.invalid_clients == 1 &&
          !memcmp(service.frontend[2].from_guest.data, "reply", 5) &&
          !memcmp(service.guest[2].to_guest.data, "insp", 4),
          "operator exchange is isolated and survives a malformed guest queue");
    req.client = 1; req.role = SERIAL_VIRT_ROLE_VMM;
    check(aos_serial_virt_detach(&service, VIRT_CLIENT_BADGE_PRIMARY, &req, sizeof(req)) ==
          SERIAL_VIRT_ERR_AUTHORITY && service.guest_attached[1],
          "foreign detach preserves peer attachment");
    req.client = 0; req.role = SERIAL_VIRT_ROLE_FRONTEND;
    check(aos_serial_virt_detach(&service, SERIAL_VIRT_FRONTEND_BADGE, &req, sizeof(req)) ==
          SERIAL_VIRT_ERR_AUTHORITY && service.guest_attached[0],
          "frontend cannot retire guest queues");
    req.client = 2; req.role = SERIAL_VIRT_ROLE_OPERATOR;
    check(aos_serial_virt_detach(&service, SERIAL_VIRT_OPERATOR_BADGE, &req, sizeof(req)) ==
          SERIAL_VIRT_ERR_AUTHORITY && service.guest_attached[2],
          "guest detach contract excludes operator channel");
    req.client = 0; req.role = SERIAL_VIRT_ROLE_VMM;
    check(aos_serial_virt_detach(&service, VIRT_CLIENT_BADGE_PRIMARY, &req, sizeof(req)-1) ==
          SERIAL_VIRT_ERR_PROTOCOL && service.guest_attached[0],
          "short detach preserves attachment");
    req.version++;
    check(aos_serial_virt_detach(&service, VIRT_CLIENT_BADGE_PRIMARY, &req, sizeof(req)) ==
          SERIAL_VIRT_ERR_VERSION && service.guest_attached[0],
          "wrong detach version preserves attachment");
    req.version = SERIAL_VIRT_CONTRACT_VERSION;
    check(aos_serial_frontend_begin(&service.frontend[0]) == AOS_SERIAL_PUMP_OK,
          "frontend can admit an operation before retirement");
    check(aos_serial_virt_detach(&service, VIRT_CLIENT_BADGE_PRIMARY, &req, sizeof(req)) ==
          SERIAL_VIRT_ERR_BUSY && service.guest_attached[0] && !service.guest_retired[0],
          "detach waits for admitted frontend access before forgetting guest pointers");
    aos_serial_frontend_end(&service.frontend[0]);
    uint8_t late = 'x'; uint32_t late_count = 99;
    uint32_t old_tail = service.frontend[0].to_guest.queue->tail;
    uint32_t old_head = service.frontend[0].from_guest.queue->head;
    check(aos_serial_frontend_write(&service.frontend[0], &late, 1) == AOS_SERIAL_PUMP_INVALID &&
          aos_serial_frontend_read(&service.frontend[0], &late, 1, &late_count) == AOS_SERIAL_PUMP_INVALID &&
          !late_count && service.frontend[0].to_guest.queue->tail == old_tail &&
          service.frontend[0].from_guest.queue->head == old_head,
          "ending an admitted operation preserves retirement and rejects late I/O without cursor changes");
    check(aos_serial_virt_detach(&service, VIRT_CLIENT_BADGE_PRIMARY, &req, sizeof(req)) ==
          SERIAL_VIRT_OK && !service.guest_attached[0] && service.guest_retired[0] &&
          !service.guest[0].meta && !service.guest[0].from_guest.queue &&
          !service.frontend[0].meta->attached,
          "terminal detach forgets guest pointers without waiting for unread bytes");
    check(mprotect(retired_page, AOS_SERIAL_FRAME_SIZE, PROT_NONE) == 0,
          "retired guest page becomes inaccessible");
    check(aos_serial_virt_detach(&service, VIRT_CLIENT_BADGE_PRIMARY, &req, sizeof(req)) ==
          SERIAL_VIRT_OK && aos_serial_virt_attach(&service, VIRT_CLIENT_BADGE_PRIMARY,
              &req, sizeof(req)) == SERIAL_VIRT_ERR_BUSY,
          "detach is idempotent and reattachment is refused");
    service.guest[1].from_guest.data[3] = '!';
    service.guest[1].from_guest.queue->tail = 4;
    service.guest[2].from_guest.data[5] = '?';
    service.guest[2].from_guest.queue->tail = 6;
    result = aos_serial_virt_service_pump(&service, 8);
    check(result.bytes == 2 && result.wake_vmm == 6 && !result.invalid_clients &&
          service.frontend[1].from_guest.data[3] == '!' &&
          service.frontend[2].from_guest.data[5] == '?',
          "peer and operator bytes survive late wake with retired memory protected");
    check(munmap(retired_page, AOS_SERIAL_FRAME_SIZE) == 0, "release test mapping");
    printf("1..%u\n", checks);
    return failures ? 1 : 0;
}
