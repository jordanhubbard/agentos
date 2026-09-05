#include "native_net_client.h"

#include <contracts/net-service/interface.h>

static void write_u32(uint8_t *dst, uint32_t off, uint32_t value)
{
    dst[off] = (uint8_t)value;
    dst[off + 1u] = (uint8_t)(value >> 8u);
    dst[off + 2u] = (uint8_t)(value >> 16u);
    dst[off + 3u] = (uint8_t)(value >> 24u);
}

static uint32_t read_u32(const uint8_t *src, uint32_t off)
{
    return (uint32_t)src[off] |
           ((uint32_t)src[off + 1u] << 8u) |
           ((uint32_t)src[off + 2u] << 16u) |
           ((uint32_t)src[off + 3u] << 24u);
}

static bool invoke(native_net_client_t *client, uint32_t opcode,
                   uint32_t arg0, uint32_t arg1, sel4_msg_t *rep)
{
    sel4_msg_t req = {0};

    if (client == NULL || client->call == NULL || rep == NULL) return false;
    req.opcode = opcode;
    req.length = 8u;
    write_u32(req.data, 0u, arg0);
    write_u32(req.data, 4u, arg1);
    client->call(client->endpoint, &req, rep);
    return rep->opcode == 0u &&
           rep->length >= sizeof(uint32_t) &&
           read_u32(rep->data, 0u) == NET_SVC_RAW_OK;
}

bool native_net_client_open(native_net_client_t *client, uintptr_t endpoint,
                            void *shared, size_t shared_size,
                            uint32_t interface_id,
                            native_net_call_fn call)
{
    sel4_msg_t rep = {0};

    if (client == NULL || endpoint == 0u || shared == NULL ||
        call == NULL || shared_size < NET_SVC_SLOT_SIZE) {
        return false;
    }
    *client = (native_net_client_t){
        .endpoint = endpoint,
        .shared = shared,
        .shared_size = shared_size,
        .call = call,
    };
    if (!invoke(client, NET_SVC_OP_RAW_OPEN, interface_id, 0u, &rep) ||
        rep.length < 12u) {
        return false;
    }
    client->handle = read_u32(rep.data, 4u);
    client->slot_offset = read_u32(rep.data, 8u);
    if (client->slot_offset < NET_SVC_SLOT_BASE ||
        client->slot_offset > shared_size ||
        NET_SVC_SLOT_SIZE > shared_size - client->slot_offset) {
        return false;
    }
    client->ready = true;
    return true;
}

bool native_net_client_status(native_net_client_t *client, bool *link_up)
{
    sel4_msg_t rep = {0};
    if (client == NULL || !client->ready || link_up == NULL ||
        !invoke(client, NET_SVC_OP_RAW_STATUS,
                client->handle, 0u, &rep) || rep.length < 8u) {
        return false;
    }
    *link_up = read_u32(rep.data, 4u) != 0u;
    return true;
}

bool native_net_client_send(native_net_client_t *client,
                            const void *frame, uint32_t length)
{
    sel4_msg_t rep = {0};
    const uint8_t *src = frame;
    uint8_t *dst;

    if (client == NULL || !client->ready || frame == NULL ||
        length == 0u || length > NET_SVC_MAX_FRAME_BYTES) {
        return false;
    }
    dst = client->shared + client->slot_offset + NET_SVC_HDR_SIZE;
    for (uint32_t i = 0u; i < length; i++) dst[i] = src[i];
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return invoke(client, NET_SVC_OP_RAW_SEND,
                  client->handle, length, &rep);
}

int native_net_client_recv(native_net_client_t *client,
                           void *frame, uint32_t capacity)
{
    sel4_msg_t rep = {0};
    uint8_t *dst = frame;
    uint32_t length;
    uint32_t offset;

    if (client == NULL || !client->ready || frame == NULL ||
        capacity == 0u ||
        !invoke(client, NET_SVC_OP_RAW_RECV,
                client->handle, capacity, &rep) ||
        rep.length < 12u) {
        return -1;
    }
    length = read_u32(rep.data, 4u);
    offset = read_u32(rep.data, 8u);
    if (length > capacity || offset > client->shared_size ||
        length > client->shared_size - offset) {
        return -1;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (uint32_t i = 0u; i < length; i++) dst[i] = client->shared[offset + i];
    return (int)length;
}
