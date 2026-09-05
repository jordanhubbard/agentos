/*
 * Native-agent client for the canonical net-service raw-device contract.
 *
 * This is the non-guest peer of platform/net-virt/vmm_virtio_net.c: both use
 * the same net_pd endpoint and shared-memory slots, but this client presents
 * packets directly to an agent PD instead of emulating a VirtIO device.
 */
#ifndef AGENTOS_NATIVE_NET_CLIENT_H
#define AGENTOS_NATIVE_NET_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sel4_msg_types.h"

typedef void (*native_net_call_fn)(uintptr_t endpoint,
                                   const sel4_msg_t *req,
                                   sel4_msg_t *rep);

typedef struct native_net_client {
    uintptr_t endpoint;
    uint8_t *shared;
    size_t shared_size;
    uint32_t handle;
    uint32_t slot_offset;
    native_net_call_fn call;
    bool ready;
} native_net_client_t;

bool native_net_client_open(native_net_client_t *client, uintptr_t endpoint,
                            void *shared, size_t shared_size,
                            uint32_t interface_id,
                            native_net_call_fn call);
bool native_net_client_status(native_net_client_t *client, bool *link_up);
bool native_net_client_send(native_net_client_t *client,
                            const void *frame, uint32_t length);
int native_net_client_recv(native_net_client_t *client,
                           void *frame, uint32_t capacity);

#endif /* AGENTOS_NATIVE_NET_CLIENT_H */
