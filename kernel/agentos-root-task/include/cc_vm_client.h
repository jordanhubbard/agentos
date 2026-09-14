#pragma once
#include "contracts/cc_contract.h"
#include <stdbool.h>
#include <stdint.h>

/* Public CC handles are monotonic and independent of reusable backend slots.
 * A failed START is rolled back. If rollback fails, CREATE returns its retained
 * handle for recovery; the entry remains visible to LIST and DESTROY. */
#define CC_VM_CLIENT_SLOTS 4u
#define CC_VM_PAYLOAD_BYTES 48u
typedef struct {
  uint32_t status, length;
  uint8_t data[CC_VM_PAYLOAD_BYTES];
} cc_vm_reply_t;
typedef void (*cc_vm_rpc_t)(uint32_t opcode, const uint8_t *data,
                            uint32_t length, cc_vm_reply_t *reply, void *ctx);
typedef struct {
  uint32_t handle, slot, profile, devices;
  bool active;
} cc_vm_entry_t;
typedef struct {
  cc_vm_entry_t entries[CC_VM_CLIENT_SLOTS];
  uint32_t next_handle;
  cc_vm_rpc_t rpc;
  void *ctx;
} cc_vm_client_t;

void cc_vm_client_init(cc_vm_client_t *, cc_vm_rpc_t, void *);
uint32_t cc_vm_create(cc_vm_client_t *, uint32_t profile, uint32_t ram_mb,
                      uint32_t devices, uint32_t *handle);
uint32_t cc_vm_status(cc_vm_client_t *, uint32_t handle, cc_guest_status_t *);
uint32_t cc_vm_lifecycle(cc_vm_client_t *, uint32_t opcode, uint32_t handle,
                         uint32_t *state);
uint32_t cc_vm_request(cc_vm_client_t *, uint32_t opcode, uint32_t handle,
                       const uint8_t *tail, uint32_t tail_len, cc_vm_reply_t *);
uint32_t cc_vm_list(cc_vm_client_t *, cc_guest_info_t *, uint32_t max);
