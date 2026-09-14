#include "cc_vm_client.h"
#include "contracts/guest_contract.h"
#include "contracts/vibeos_contract.h"
#include "contracts/vm_manager_contract.h"
#include <stdio.h>
#include <string.h>

static unsigned tests, failures;
#define CHECK(x)                                                               \
  do {                                                                         \
    bool passed = (x);                                                         \
    ++tests;                                                                   \
    if (!passed)                                                               \
      ++failures;                                                              \
    printf("%s %u - %s\n", passed ? "ok" : "not ok", tests, #x);               \
  } while (0)
static uint32_t rd(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static void wr(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (i * 8));
}
typedef struct {
  unsigned calls, destroys;
  uint32_t state[2], flags[2], ram[2];
  bool fail_start, fail_destroy, short_create, short_info, wrong_profile,
      alias_reply;
  uint32_t last_op, last_slot, last_length;
  uint8_t last_data[48];
} backend_t;
static void rpc(uint32_t op, const uint8_t *p, uint32_t n, cc_vm_reply_t *r,
                void *ctx) {
  backend_t *b = ctx;
  b->calls++;
  b->last_op = op;
  b->last_slot = rd(p);
  b->last_length = n;
  memcpy(b->last_data, p, n);
  r->length = 4;
  wr(r->data, 0);
  uint32_t slot = rd(p);
  if (slot >= 2) {
    r->status = 1;
    return;
  }
  if (op == VM_MANAGER_OP_CREATE) {
    if (n != 12 || b->state[slot]) {
      r->status = 1;
      return;
    }
    if (b->alias_reply) {
      r->length = 8;
      wr(r->data + 4, 0);
      return;
    }
    if (b->short_create) {
      r->length = 4;
      return;
    }
    b->state[slot] = VM_WIRE_SLOT_BOOTING;
    b->ram[slot] = rd(p + 4);
    b->flags[slot] = rd(p + 8);
    r->length = 8;
    wr(r->data + 4, slot);
    return;
  }
  if (!b->state[slot]) {
    r->status = 1;
    return;
  }
  switch (op) {
  case VM_MANAGER_OP_START:
    if (b->fail_start) {
      wr(r->data, 1);
      return;
    }
    b->state[slot] = VM_WIRE_SLOT_RUNNING;
    break;
  case VM_MANAGER_OP_DESTROY:
    b->destroys++;
    if (b->fail_destroy) {
      r->status = 5;
      return;
    }
    b->state[slot] = 0;
    break;
  case VM_MANAGER_OP_STOP:
    b->state[slot] = VM_WIRE_SLOT_SUSPENDED;
    break;
  case VM_MANAGER_OP_RESUME:
    b->state[slot] = VM_WIRE_SLOT_RUNNING;
    break;
  case VM_MANAGER_OP_INFO:
    r->length = b->short_info ? 4 : 48;
    wr(r->data + 4, b->state[slot]);
    wr(r->data + 8, b->wrong_profile ? 1 - slot : slot);
    wr(r->data + 12, b->ram[slot]);
    wr(r->data + 16, 1);
    wr(r->data + 44, b->flags[slot]);
    break;
  case VM_MANAGER_OP_SNAPSHOT:
    wr(r->data, 2);
    break;
  default:
    break;
  }
}
int main(void) {
  backend_t b = {0};
  cc_vm_client_t c;
  cc_vm_client_init(&c, rpc, &b);
  uint32_t first = 0, second = 0, state = 0;
  cc_guest_status_t st;
  cc_guest_info_t list[8];
  uint32_t rc =
      cc_vm_create(&c, 1, 256, VIBEOS_DEV_SERIAL | VIBEOS_DEV_NET, &first);
  CHECK(rc == CC_OK && first == 1 && b.state[0] == VM_WIRE_SLOT_RUNNING);
  CHECK(b.flags[0] == (VM_CREATE_FLAG_SERIAL | VM_CREATE_FLAG_VIRTIO_NET));
  unsigned before = b.calls;
  rc = cc_vm_create(&c, 1, 256, 0, &second);
  CHECK(rc != CC_OK && second == 0 && b.calls == before);
  b.alias_reply = true;
  CHECK(cc_vm_create(&c, 2, 512, 0, &second) != CC_OK && second == 0 &&
        b.last_op == VM_MANAGER_OP_CREATE);
  CHECK(b.state[0] == VM_WIRE_SLOT_RUNNING && b.destroys == 0);
  b.alias_reply = false;
  rc = cc_vm_create(&c, 2, 512, VIBEOS_DEV_BLOCK, &second);
  CHECK(rc == CC_OK && second != first &&
        b.flags[1] == VM_CREATE_FLAG_VIRTIO_BLK);
  rc = cc_vm_status(&c, second, &st);
  CHECK(rc == CC_OK && st.guest_handle == second && st.os_type == 2 &&
        st.state == GUEST_STATE_RUNNING && st.device_flags == VIBEOS_DEV_BLOCK);
  CHECK(cc_vm_list(&c, list, 1) == 1 && list[0].guest_handle == first);
  rc = cc_vm_lifecycle(&c, VM_MANAGER_OP_STOP, first, &state);
  CHECK(rc == CC_OK && state == GUEST_STATE_SUSPENDED);
  rc = cc_vm_status(&c, first, &st);
  CHECK(rc == CC_OK && st.state == GUEST_STATE_SUSPENDED);
  rc = cc_vm_lifecycle(&c, VM_MANAGER_OP_RESUME, first, &state);
  CHECK(rc == CC_OK && state == GUEST_STATE_RUNNING);
  uint8_t input[44];
  memset(input, 0x5a, sizeof(input));
  cc_vm_reply_t reply;
  rc = cc_vm_request(&c, VM_MANAGER_OP_SEND_INPUT, second, input, sizeof(input),
                     &reply);
  CHECK(rc == CC_OK && b.last_slot == 1 && b.last_length == 48 &&
        b.last_data[47] == 0x5a);
  before = b.calls;
  CHECK(cc_vm_request(&c, VM_MANAGER_OP_SEND_INPUT, second, input, 45,
                      &reply) != CC_OK &&
        b.calls == before);
  CHECK(cc_vm_request(&c, VM_MANAGER_OP_SNAPSHOT, first, NULL, 0, &reply) !=
        CC_OK);
  b.short_info = true;
  CHECK(cc_vm_status(&c, first, &st) != CC_OK);
  b.short_info = false;
  b.wrong_profile = true;
  CHECK(cc_vm_status(&c, first, &st) != CC_OK);
  b.wrong_profile = false;
  b.fail_destroy = true;
  CHECK(cc_vm_lifecycle(&c, VM_MANAGER_OP_DESTROY, first, NULL) != CC_OK);
  CHECK(cc_vm_status(&c, first, &st) == CC_OK);
  b.fail_destroy = false;
  CHECK(cc_vm_lifecycle(&c, VM_MANAGER_OP_DESTROY, first, NULL) == CC_OK);
  before = b.calls;
  CHECK(cc_vm_status(&c, first, &st) == CC_ERR_BAD_HANDLE && b.calls == before);
  uint32_t third = 0;
  CHECK(cc_vm_create(&c, 1, 256, 0, &third) == CC_OK && third > second);
  CHECK(cc_vm_status(&c, first, &st) == CC_ERR_BAD_HANDLE);
  CHECK(cc_vm_lifecycle(&c, VM_MANAGER_OP_DESTROY, third, NULL) == CC_OK);
  b.fail_start = true;
  unsigned destroyed = b.destroys;
  CHECK(cc_vm_create(&c, 1, 256, 0, &third) != CC_OK && third == 0 &&
        b.destroys == destroyed + 1);
  b.fail_destroy = true;
  CHECK(cc_vm_create(&c, 1, 256, 0, &third) != CC_OK && third != 0);
  CHECK(cc_vm_status(&c, third, &st) == CC_OK &&
        st.state == GUEST_STATE_BOOTING);
  CHECK(cc_vm_list(&c, list, 8) == 2);
  b.fail_destroy = false;
  CHECK(cc_vm_lifecycle(&c, VM_MANAGER_OP_DESTROY, third, NULL) == CC_OK);
  b.fail_start = false;
  b.short_create = true;
  CHECK(cc_vm_create(&c, 1, 256, 0, &third) != CC_OK && third == 0);
  b.short_create = false;
  c.next_handle = 0;
  before = b.calls;
  CHECK(cc_vm_create(&c, 1, 256, 0, &third) != CC_OK && b.calls == before);
  c.next_handle = 20;
  CHECK(cc_vm_create(&c, 99, 256, 0, &third) != CC_OK && b.calls == before);
  CHECK(cc_vm_create(&c, 1, 65, 0, &third) != CC_OK && b.calls == before);
  CHECK(cc_vm_create(&c, 1, 256, VIBEOS_DEV_USB, &third) != CC_OK &&
        b.calls == before);
  printf("1..%u\n", tests);
  return failures ? 1 : 0;
}
