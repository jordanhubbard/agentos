#include "cc_vm_client.h"
#include "contracts/guest_contract.h"
#include "contracts/vibeos_contract.h"
#include "contracts/vm_manager_contract.h"
#include <string.h>

static uint32_t rd(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static void wr(uint8_t *p, uint32_t x) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (uint8_t)(x >> (i * 8));
}
static cc_vm_entry_t *lookup(cc_vm_client_t *c, uint32_t h) {
  if (!h)
    return NULL;
  for (unsigned i = 0; i < CC_VM_CLIENT_SLOTS; i++)
    if (c->entries[i].active && c->entries[i].handle == h)
      return &c->entries[i];
  return NULL;
}
static uint32_t call(cc_vm_client_t *c, uint32_t op, const uint8_t *p,
                     uint32_t n, cc_vm_reply_t *r, uint32_t minimum) {
  memset(r, 0, sizeof(*r));
  if (!c->rpc || n > CC_VM_PAYLOAD_BYTES)
    return CC_ERR_RELAY_FAULT;
  c->rpc(op, p, n, r, c->ctx);
  if (r->status || r->length < minimum || r->length > CC_VM_PAYLOAD_BYTES ||
      rd(r->data))
    return CC_ERR_RELAY_FAULT;
  return CC_OK;
}
void cc_vm_client_init(cc_vm_client_t *c, cc_vm_rpc_t rpc, void *ctx) {
  memset(c, 0, sizeof(*c));
  c->next_handle = 1;
  c->rpc = rpc;
  c->ctx = ctx;
}
uint32_t cc_vm_request(cc_vm_client_t *c, uint32_t op, uint32_t h,
                       const uint8_t *tail, uint32_t n, cc_vm_reply_t *r) {
  cc_vm_entry_t *e = lookup(c, h);
  if (!e)
    return CC_ERR_BAD_HANDLE;
  if (n > CC_VM_PAYLOAD_BYTES - 4 || (n && !tail))
    return CC_ERR_RELAY_FAULT;
  uint8_t p[CC_VM_PAYLOAD_BYTES] = {0};
  wr(p, e->slot);
  if (n)
    memcpy(p + 4, tail, n);
  return call(c, op, p, n + 4, r, 4);
}
uint32_t cc_vm_create(cc_vm_client_t *c, uint32_t profile, uint32_t ram,
                      uint32_t devices, uint32_t *handle) {
  *handle = 0;
  const uint32_t allowed =
      VIBEOS_DEV_SERIAL | VIBEOS_DEV_NET | VIBEOS_DEV_BLOCK;
  if (profile < VIBEOS_PROFILE_PRIMARY || profile > VIBEOS_PROFILE_SECONDARY ||
      ram < 64 || ram > 8192 || (ram & 3) || (devices & ~allowed))
    return CC_ERR_RELAY_FAULT;
  cc_vm_entry_t *e = NULL;
  for (unsigned i = 0; i < CC_VM_CLIENT_SLOTS; i++) {
    if (c->entries[i].active && c->entries[i].profile == profile)
      return CC_ERR_RELAY_FAULT;
    if (!c->entries[i].active && !e)
      e = &c->entries[i];
  }
  if (!e || !c->next_handle)
    return CC_ERR_RELAY_FAULT;
  uint32_t flags = 0;
  if (devices & VIBEOS_DEV_SERIAL)
    flags |= VM_CREATE_FLAG_SERIAL;
  if (devices & VIBEOS_DEV_NET)
    flags |= VM_CREATE_FLAG_VIRTIO_NET;
  if (devices & VIBEOS_DEV_BLOCK)
    flags |= VM_CREATE_FLAG_VIRTIO_BLK;
  uint8_t p[12];
  wr(p, profile - 1);
  wr(p + 4, ram);
  wr(p + 8, flags);
  cc_vm_reply_t r;
  uint32_t rc = call(c, VM_MANAGER_OP_CREATE, p, sizeof(p), &r, 8);
  if (rc != CC_OK)
    return rc;
  uint32_t slot = rd(r.data + 4);
  /* A malformed/aliased backend reply never grants a second public handle. */
  if (slot >= CC_VM_CLIENT_SLOTS)
    return CC_ERR_RELAY_FAULT;
  for (unsigned i = 0; i < CC_VM_CLIENT_SLOTS; i++)
    if (c->entries[i].active && c->entries[i].slot == slot)
      return CC_ERR_RELAY_FAULT;
  *e = (cc_vm_entry_t){.active = true,
                       .handle = c->next_handle++,
                       .slot = slot,
                       .profile = profile,
                       .devices = devices};
  rc = cc_vm_request(c, VM_MANAGER_OP_START, e->handle, NULL, 0, &r);
  if (rc != CC_OK) {
    cc_vm_reply_t rollback;
    if (cc_vm_request(c, VM_MANAGER_OP_DESTROY, e->handle, NULL, 0,
                      &rollback) == CC_OK)
      e->active = false;
    else
      *handle = e->handle;
    return rc;
  }
  *handle = e->handle;
  return CC_OK;
}
uint32_t cc_vm_status(cc_vm_client_t *c, uint32_t h, cc_guest_status_t *out) {
  cc_vm_reply_t r;
  uint32_t rc = cc_vm_request(c, VM_MANAGER_OP_INFO, h, NULL, 0, &r);
  if (rc != CC_OK)
    return rc;
  if (r.length != sizeof(vm_manager_reply_info_t))
    return CC_ERR_RELAY_FAULT;
  cc_vm_entry_t *e = lookup(c, h);
  uint32_t slot_state = rd(r.data + 4);
  if (rd(r.data + 8) != e->profile - 1 || slot_state > VM_WIRE_SLOT_ERROR)
    return CC_ERR_RELAY_FAULT;
  memset(out, 0, sizeof(*out));
  out->guest_handle = h;
  out->os_type = e->profile;
#if defined(__x86_64__)
  out->arch = VIBEOS_ARCH_X86_64;
#else
  out->arch = VIBEOS_ARCH_AARCH64;
#endif
  switch (slot_state) {
  case VM_WIRE_SLOT_BOOTING:
    out->state = GUEST_STATE_BOOTING;
    break;
  case VM_WIRE_SLOT_RUNNING:
    out->state = GUEST_STATE_RUNNING;
    break;
  case VM_WIRE_SLOT_SUSPENDED:
    out->state = GUEST_STATE_SUSPENDED;
    break;
  default:
    out->state = GUEST_STATE_DEAD;
    break;
  }
  uint32_t flags = rd(r.data + 44);
  if (flags & VM_CREATE_FLAG_SERIAL)
    out->device_flags |= VIBEOS_DEV_SERIAL;
  if (flags & VM_CREATE_FLAG_VIRTIO_NET)
    out->device_flags |= VIBEOS_DEV_NET;
  if (flags & VM_CREATE_FLAG_VIRTIO_BLK)
    out->device_flags |= VIBEOS_DEV_BLOCK;
  return CC_OK;
}
uint32_t cc_vm_lifecycle(cc_vm_client_t *c, uint32_t op, uint32_t h,
                         uint32_t *state) {
  if (op != VM_MANAGER_OP_STOP && op != VM_MANAGER_OP_RESUME &&
      op != VM_MANAGER_OP_DESTROY)
    return CC_ERR_RELAY_FAULT;
  cc_vm_reply_t r;
  uint32_t rc = cc_vm_request(c, op, h, NULL, 0, &r);
  if (rc != CC_OK)
    return rc;
  if (op == VM_MANAGER_OP_DESTROY)
    lookup(c, h)->active = false;
  if (state)
    *state = op == VM_MANAGER_OP_STOP     ? GUEST_STATE_SUSPENDED
             : op == VM_MANAGER_OP_RESUME ? GUEST_STATE_RUNNING
                                          : GUEST_STATE_DEAD;
  return CC_OK;
}
uint32_t cc_vm_list(cc_vm_client_t *c, cc_guest_info_t *out, uint32_t max) {
  uint32_t n = 0;
  for (unsigned i = 0; i < CC_VM_CLIENT_SLOTS && n < max; i++) {
    if (!c->entries[i].active)
      continue;
    cc_guest_status_t status;
    if (cc_vm_status(c, c->entries[i].handle, &status) != CC_OK)
      continue;
    out[n++] = (cc_guest_info_t){.guest_handle = status.guest_handle,
                                 .state = status.state,
                                 .os_type = status.os_type,
                                 .arch = status.arch};
  }
  return n;
}
