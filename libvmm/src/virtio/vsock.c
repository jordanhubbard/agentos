/*
 * libvmm/virtio/vsock.c — virtio-vsock device backend
 *
 * Implements the host side of a virtio-vsock transport (Virtio 1.2 §5.10).
 * The guest sees a stock virtio-vsock device on the MMIO bus; this code
 * marshals AF_VSOCK packets in and out of the TX/RX virtqueues and
 * dispatches them through a backend callback table.
 *
 * Connection model (subset of the spec sufficient for control-plane
 * traffic; high-throughput data-plane callers should re-evaluate):
 *
 *   - We track up to VIRTIO_VSOCK_MAX_CONNS active 4-tuples in a flat
 *     array; lookup is O(N) but N is small.
 *   - The guest is the *only* connection initiator in v1: it sends
 *     OP_REQUEST and we either accept (-> OP_RESPONSE) or reject
 *     (-> OP_RST). Host-initiated connections are out of scope; the
 *     backend can always reach the guest via virtio_vsock_send() once
 *     a connection exists.
 *   - We advertise infinite RX buf_alloc (UINT32_MAX). This sidesteps
 *     credit-update bookkeeping for v1; the guest's vsock driver treats
 *     infinite credit as "always OK to send". We do honour the guest's
 *     advertised buf_alloc/fwd_cnt so we don't overrun its RX ring.
 *   - SEQPACKET and DGRAM are not supported. STREAM only.
 *
 * Copyright 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>
#include <libvmm/vmm_caps.h>
#include <libvmm/guest.h>
#include <libvmm/virq.h>
#include <libvmm/util/util.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/mmio.h>
#include <libvmm/virtio/virtq.h>
#include <libvmm/virtio/virtio.h>
#include <libvmm/virtio/vsock.h>

// #define DEBUG_VSOCK

#if defined(DEBUG_VSOCK)
#define LOG_VSOCK(...) do{ printf("VIRTIO(VSOCK): "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_VSOCK(...) do{}while(0)
#endif

#define LOG_VSOCK_ERR(...) do{ printf("VIRTIO(VSOCK)|ERROR: "); printf(__VA_ARGS__); }while(0)

static inline struct virtio_vsock_device *device_state(struct virtio_device *dev)
{
    return (struct virtio_vsock_device *)dev->device_data;
}

/* ── Connection table helpers ──────────────────────────────────────── */

static struct virtio_vsock_conn *conn_lookup(struct virtio_vsock_device *vs,
                                             uint64_t peer_cid, uint32_t peer_port,
                                             uint32_t local_port)
{
    for (size_t i = 0; i < VIRTIO_VSOCK_MAX_CONNS; i++) {
        struct virtio_vsock_conn *c = &vs->conns[i];
        if (!c->in_use) continue;
        if (c->peer_cid == peer_cid &&
            c->peer_port == peer_port &&
            c->local_port == local_port) {
            return c;
        }
    }
    return NULL;
}

static struct virtio_vsock_conn *conn_alloc(struct virtio_vsock_device *vs)
{
    for (size_t i = 0; i < VIRTIO_VSOCK_MAX_CONNS; i++) {
        if (!vs->conns[i].in_use) {
            memset(&vs->conns[i], 0, sizeof(vs->conns[i]));
            vs->conns[i].in_use = true;
            vs->conns[i].state = VSOCK_CONN_CLOSED;
            vs->conns[i].local_cid = VIRTIO_VSOCK_CID_HOST;
            return &vs->conns[i];
        }
    }
    return NULL;
}

static void conn_free(struct virtio_vsock_conn *c)
{
    c->in_use = false;
    c->state = VSOCK_CONN_CLOSED;
}

/* ── RX queue: push a packet to the guest ──────────────────────────── */

/* Common helper used by both virtio_vsock_send() (data) and the internal
 * synthesizers (RESPONSE, RST, SHUTDOWN). Pops one RX descriptor, writes
 * a hdr+payload pair into it, advances used, sets IRQ status.
 *
 * Returns false if RX queue is empty; caller may retry. */
static bool rx_push(struct virtio_vsock_device *vs,
                    const struct virtio_vsock_hdr *hdr,
                    const void *payload, uint32_t len)
{
    struct virtio_queue_handler *vq = &vs->virtio_device.vqs[VIRTIO_VSOCK_QUEUE_RX];
    if (!vq->ready) return false;
    if (vq->last_idx == vq->virtq.avail->idx) {
        LOG_VSOCK("rx queue empty, dropping packet (op=%u len=%u)\n", hdr->op, len);
        return false;
    }

    uint16_t desc_head = vq->virtq.avail->ring[vq->last_idx % vq->virtq.num];
    uint32_t total = (uint32_t)sizeof(*hdr) + len;

    /* Walk the chain accumulating writeable space. */
    uint32_t written = 0;
    uint16_t desc_idx = desc_head;
    bool ok = false;
    do {
        struct virtq_desc desc = vq->virtq.desc[desc_idx];
        if (!(desc.flags & VIRTQ_DESC_F_WRITE)) {
            LOG_VSOCK_ERR("rx descriptor 0x%x not writeable\n", desc_idx);
            break;
        }
        uint32_t want = total - written;
        uint32_t take = (desc.len < want) ? desc.len : want;

        if (written < sizeof(*hdr)) {
            /* Header fragment in this descriptor. */
            uint32_t hdr_off = written;
            uint32_t hdr_take = (sizeof(*hdr) - hdr_off);
            if (hdr_take > take) hdr_take = take;
            memcpy((void *)(uintptr_t)desc.addr,
                   (const uint8_t *)hdr + hdr_off, hdr_take);
            written += hdr_take;
            take -= hdr_take;
            if (take && payload) {
                memcpy((void *)(uintptr_t)(desc.addr + hdr_take),
                       payload, take);
                written += take;
            }
        } else {
            uint32_t pay_off = written - sizeof(*hdr);
            memcpy((void *)(uintptr_t)desc.addr,
                   (const uint8_t *)payload + pay_off, take);
            written += take;
        }
        if (written == total) { ok = true; break; }
        if (!(desc.flags & VIRTQ_DESC_F_NEXT)) break;
        desc_idx = desc.next;
    } while (true);

    if (!ok) {
        LOG_VSOCK_ERR("rx descriptor chain too small for packet (need %u, got %u)\n",
                      total, written);
        /* Still mark used with len=0 so the guest can recycle the buffer. */
        written = 0;
    }

    struct virtq_used_elem used = { .id = desc_head, .len = written };
    vs->virtio_device.vqs[VIRTIO_VSOCK_QUEUE_RX]
        .virtq.used->ring[vq->virtq.used->idx % vq->virtq.num] = used;
    vq->virtq.used->idx++;
    vq->last_idx++;

    vs->virtio_device.regs.InterruptStatus = BIT_LOW(0);
    bool inj = virq_inject(vs->virtio_device.virq);
    assert(inj);
    return ok;
}

/* Build a header for a host-originated packet on `conn`. */
static void hdr_for_conn(const struct virtio_vsock_conn *conn,
                         struct virtio_vsock_hdr *hdr,
                         uint16_t op, uint32_t len, uint32_t flags)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->src_cid = conn->local_cid;
    hdr->dst_cid = conn->peer_cid;
    hdr->src_port = conn->local_port;
    hdr->dst_port = conn->peer_port;
    hdr->len = len;
    hdr->type = conn->type ? conn->type : VIRTIO_VSOCK_TYPE_STREAM;
    hdr->op = op;
    hdr->flags = flags;
    hdr->buf_alloc = UINT32_MAX;  /* see file header: infinite credit */
    hdr->fwd_cnt = conn->rx_cnt;
}

/* ── TX queue: process a guest-originated packet ────────────────────── */

static void tx_handle_packet(struct virtio_vsock_device *vs,
                             const struct virtio_vsock_hdr *hdr,
                             const void *payload, uint32_t paylen)
{
    /* Look up an existing connection by 4-tuple. For the (peer_cid,
     * peer_port, local_port) key the guest is the peer. */
    struct virtio_vsock_conn *conn =
        conn_lookup(vs, hdr->src_cid, hdr->src_port, hdr->dst_port);

    switch (hdr->op) {
    case VIRTIO_VSOCK_OP_REQUEST: {
        if (conn) {
            /* Duplicate REQUEST on existing 4-tuple: reset. */
            struct virtio_vsock_hdr rst;
            hdr_for_conn(conn, &rst, VIRTIO_VSOCK_OP_RST, 0, 0);
            rx_push(vs, &rst, NULL, 0);
            if (vs->ops && vs->ops->on_close) {
                vs->ops->on_close(conn, vs->backend_user);
            }
            conn_free(conn);
            break;
        }
        conn = conn_alloc(vs);
        if (!conn) {
            /* No slot: synthesise RST manually (no conn for hdr_for_conn). */
            struct virtio_vsock_hdr rst = {
                .src_cid = hdr->dst_cid, .dst_cid = hdr->src_cid,
                .src_port = hdr->dst_port, .dst_port = hdr->src_port,
                .type = hdr->type, .op = VIRTIO_VSOCK_OP_RST,
                .buf_alloc = UINT32_MAX,
            };
            rx_push(vs, &rst, NULL, 0);
            break;
        }
        conn->peer_cid = hdr->src_cid;
        conn->peer_port = hdr->src_port;
        conn->local_port = hdr->dst_port;
        conn->type = hdr->type;
        conn->peer_buf_alloc = hdr->buf_alloc;
        conn->peer_fwd_cnt = hdr->fwd_cnt;
        conn->state = VSOCK_CONN_CONNECTING;

        bool accept = vs->ops && vs->ops->on_connect &&
                      vs->ops->on_connect(conn, vs->backend_user);
        struct virtio_vsock_hdr reply;
        if (accept) {
            conn->state = VSOCK_CONN_ESTABLISHED;
            hdr_for_conn(conn, &reply, VIRTIO_VSOCK_OP_RESPONSE, 0, 0);
        } else {
            hdr_for_conn(conn, &reply, VIRTIO_VSOCK_OP_RST, 0, 0);
        }
        rx_push(vs, &reply, NULL, 0);
        if (!accept) conn_free(conn);
        break;
    }

    case VIRTIO_VSOCK_OP_RW: {
        if (!conn || conn->state != VSOCK_CONN_ESTABLISHED) {
            LOG_VSOCK("RW on unknown/closed conn (peer %lu:%u -> :%u), RST\n",
                      hdr->src_cid, hdr->src_port, hdr->dst_port);
            struct virtio_vsock_hdr rst = {
                .src_cid = hdr->dst_cid, .dst_cid = hdr->src_cid,
                .src_port = hdr->dst_port, .dst_port = hdr->src_port,
                .type = hdr->type, .op = VIRTIO_VSOCK_OP_RST,
                .buf_alloc = UINT32_MAX,
            };
            rx_push(vs, &rst, NULL, 0);
            break;
        }
        conn->peer_buf_alloc = hdr->buf_alloc;
        conn->peer_fwd_cnt = hdr->fwd_cnt;
        conn->rx_cnt += paylen;
        bool ok = vs->ops && vs->ops->on_recv &&
                  vs->ops->on_recv(conn, payload, paylen, vs->backend_user);
        if (!ok) {
            struct virtio_vsock_hdr rst;
            hdr_for_conn(conn, &rst, VIRTIO_VSOCK_OP_RST, 0, 0);
            rx_push(vs, &rst, NULL, 0);
            if (vs->ops && vs->ops->on_close) {
                vs->ops->on_close(conn, vs->backend_user);
            }
            conn_free(conn);
        }
        break;
    }

    case VIRTIO_VSOCK_OP_SHUTDOWN: {
        if (!conn) break;
        if (vs->ops && vs->ops->on_close) {
            vs->ops->on_close(conn, vs->backend_user);
        }
        struct virtio_vsock_hdr rst;
        hdr_for_conn(conn, &rst, VIRTIO_VSOCK_OP_RST, 0, 0);
        rx_push(vs, &rst, NULL, 0);
        conn_free(conn);
        break;
    }

    case VIRTIO_VSOCK_OP_RST: {
        if (!conn) break;
        if (vs->ops && vs->ops->on_close) {
            vs->ops->on_close(conn, vs->backend_user);
        }
        conn_free(conn);
        break;
    }

    case VIRTIO_VSOCK_OP_RESPONSE:
        /* Host-initiated connections aren't supported in v1; ignore. */
        LOG_VSOCK("ignoring guest-originated RESPONSE\n");
        break;

    case VIRTIO_VSOCK_OP_CREDIT_UPDATE:
        if (conn) {
            conn->peer_buf_alloc = hdr->buf_alloc;
            conn->peer_fwd_cnt = hdr->fwd_cnt;
        }
        break;

    case VIRTIO_VSOCK_OP_CREDIT_REQUEST: {
        if (!conn) break;
        struct virtio_vsock_hdr upd;
        hdr_for_conn(conn, &upd, VIRTIO_VSOCK_OP_CREDIT_UPDATE, 0, 0);
        rx_push(vs, &upd, NULL, 0);
        break;
    }

    default:
        LOG_VSOCK_ERR("unknown op %u\n", hdr->op);
        break;
    }
}

/* Coalesce a TX descriptor chain into a stack-bounded scratch buffer
 * (header + bounded payload), then dispatch. Anything bigger than
 * VSOCK_MAX_PACKET is RST'd.
 *
 * VSOCK_MAX_PACKET sized for control plane: 4KB exec request bundles
 * comfortably fit. */
#define VSOCK_MAX_PACKET  (sizeof(struct virtio_vsock_hdr) + 4096)

static bool tx_handle_chain(struct virtio_vsock_device *vs,
                            uint16_t desc_head, uint32_t *out_len)
{
    struct virtio_queue_handler *vq = &vs->virtio_device.vqs[VIRTIO_VSOCK_QUEUE_TX];
    uint8_t scratch[VSOCK_MAX_PACKET];
    uint32_t off = 0;
    uint16_t desc_idx = desc_head;

    do {
        struct virtq_desc desc = vq->virtq.desc[desc_idx];
        if (desc.flags & VIRTQ_DESC_F_WRITE) {
            LOG_VSOCK_ERR("tx descriptor 0x%x marked writeable\n", desc_idx);
            return false;
        }
        if (off + desc.len > sizeof(scratch)) {
            LOG_VSOCK_ERR("tx packet exceeds %zu bytes, dropping\n",
                          sizeof(scratch));
            return false;
        }
        memcpy(scratch + off, (const void *)(uintptr_t)desc.addr, desc.len);
        off += desc.len;
        if (!(desc.flags & VIRTQ_DESC_F_NEXT)) break;
        desc_idx = desc.next;
    } while (true);

    *out_len = off;
    if (off < sizeof(struct virtio_vsock_hdr)) {
        LOG_VSOCK_ERR("tx packet smaller than header (%u bytes)\n", off);
        return false;
    }

    const struct virtio_vsock_hdr *hdr = (const struct virtio_vsock_hdr *)scratch;
    uint32_t paylen = off - (uint32_t)sizeof(*hdr);
    if (hdr->len > paylen) {
        LOG_VSOCK_ERR("tx hdr->len=%u exceeds payload=%u\n", hdr->len, paylen);
        return false;
    }
    /* hdr->len <= paylen: trim trailing pad. */
    tx_handle_packet(vs, hdr, scratch + sizeof(*hdr), hdr->len);
    return true;
}

/* ── virtio_device_funs callbacks ───────────────────────────────────── */

static void virtio_vsock_reset(struct virtio_device *dev)
{
    LOG_VSOCK("operation: reset\n");
    struct virtio_vsock_device *vs = device_state(dev);
    for (int i = 0; i < dev->num_vqs; i++) {
        dev->vqs[i].ready = false;
        dev->vqs[i].last_idx = 0;
    }
    /* Tear down all connections; backend gets on_close for each. */
    for (size_t i = 0; i < VIRTIO_VSOCK_MAX_CONNS; i++) {
        if (vs->conns[i].in_use && vs->ops && vs->ops->on_close) {
            vs->ops->on_close(&vs->conns[i], vs->backend_user);
        }
        vs->conns[i].in_use = false;
    }
}

static bool virtio_vsock_get_device_features(struct virtio_device *dev, uint32_t *features)
{
    LOG_VSOCK("operation: get device features\n");
    switch (dev->regs.DeviceFeaturesSel) {
    case 0:
        /* VIRTIO_VSOCK_F_STREAM (bit 0) — Linux 6.8's vsock-virtio
         * driver insists on at least one socket-type bit; without it
         * the probe negotiation never reaches "device ready" and the
         * platform device sits in /sys/devices/platform unbound. */
        *features = BIT_LOW(VIRTIO_VSOCK_F_STREAM);
        break;
    case 1:
        *features = BIT_HIGH(VIRTIO_F_VERSION_1);
        break;
    default:
        LOG_VSOCK_ERR("bad DeviceFeaturesSel 0x%x\n", dev->regs.DeviceFeaturesSel);
        return false;
    }
    return true;
}

static bool virtio_vsock_set_driver_features(struct virtio_device *dev, uint32_t features)
{
    bool ok = false;
    switch (dev->regs.DriverFeaturesSel) {
    case 0:
        /* Driver may accept STREAM or 0 (some kernels mask down). Both ok. */
        ok = (features == 0 || features == BIT_LOW(VIRTIO_VSOCK_F_STREAM));
        break;
    case 1:
        ok = (features == BIT_HIGH(VIRTIO_F_VERSION_1));
        break;
    default:
        LOG_VSOCK_ERR("bad DriverFeaturesSel 0x%x\n", dev->regs.DriverFeaturesSel);
        return false;
    }
    if (ok) dev->features_happy = 1;
    return ok;
}

static bool virtio_vsock_get_device_config(struct virtio_device *dev, uint32_t offset, uint32_t *config)
{
    struct virtio_vsock_device *vs = device_state(dev);
    /* Only field is guest_cid (8 bytes at offset 0). */
    if (offset == 0) {
        *config = (uint32_t)(vs->config.guest_cid & 0xFFFFFFFFu);
        return true;
    }
    if (offset == 4) {
        *config = (uint32_t)(vs->config.guest_cid >> 32);
        return true;
    }
    LOG_VSOCK_ERR("bad config offset 0x%x\n", offset);
    return false;
}

static bool virtio_vsock_set_device_config(struct virtio_device *dev, uint32_t offset, uint32_t config)
{
    LOG_VSOCK_ERR("driver tried to write device config @ 0x%x = 0x%x\n", offset, config);
    return false;
}

static bool virtio_vsock_queue_notify(struct virtio_device *dev)
{
    struct virtio_vsock_device *vs = device_state(dev);
    /* Drivers notify on TX or on the event queue. We only act on TX;
     * the event queue is informational and the guest never produces
     * events on it. */
    struct virtio_queue_handler *txq = &dev->vqs[VIRTIO_VSOCK_QUEUE_TX];
    if (!txq->ready) return true;

    bool any = false;
    while (txq->last_idx != txq->virtq.avail->idx) {
        uint16_t head = txq->virtq.avail->ring[txq->last_idx % txq->virtq.num];
        uint32_t used_len = 0;
        (void)tx_handle_chain(vs, head, &used_len);
        struct virtq_used_elem used = { .id = head, .len = 0 };
        txq->virtq.used->ring[txq->virtq.used->idx % txq->virtq.num] = used;
        txq->virtq.used->idx++;
        txq->last_idx++;
        any = true;
    }
    if (any) {
        dev->regs.InterruptStatus = BIT_LOW(0);
        bool inj = virq_inject(dev->virq);
        assert(inj);
    }
    return true;
}

static virtio_device_funs_t functions = {
    .device_reset = virtio_vsock_reset,
    .get_device_features = virtio_vsock_get_device_features,
    .set_driver_features = virtio_vsock_set_driver_features,
    .get_device_config = virtio_vsock_get_device_config,
    .set_device_config = virtio_vsock_set_device_config,
    .queue_notify = virtio_vsock_queue_notify,
};

/* ── Public API ─────────────────────────────────────────────────────── */

bool virtio_mmio_vsock_init(struct virtio_vsock_device *vs,
                            uintptr_t region_base, uintptr_t region_size,
                            size_t virq, uint64_t guest_cid,
                            const virtio_vsock_backend_ops_t *ops, void *user)
{
    if (guest_cid < 3 || guest_cid == UINT32_MAX) {
        LOG_VSOCK_ERR("invalid guest_cid %lu\n", guest_cid);
        return false;
    }

    memset(vs, 0, sizeof(*vs));
    vs->config.guest_cid = guest_cid;
    vs->ops = ops;
    vs->backend_user = user;

    struct virtio_device *dev = &vs->virtio_device;
    dev->regs.DeviceID = VIRTIO_DEVICE_ID_VSOCK;
    dev->regs.VendorID = VIRTIO_MMIO_DEV_VENDOR_ID;
    dev->transport_type = VIRTIO_TRANSPORT_MMIO;
    dev->funs = &functions;
    dev->vqs = vs->vqs;
    dev->num_vqs = VIRTIO_VSOCK_NUM_VIRTQ;
    dev->virq = virq;
    dev->device_data = vs;

    return virtio_mmio_register_device(dev, region_base, region_size, virq);
}

bool virtio_vsock_send(struct virtio_vsock_device *vs,
                       struct virtio_vsock_conn *conn,
                       const void *payload, uint32_t len)
{
    if (!conn->in_use || conn->state != VSOCK_CONN_ESTABLISHED) {
        LOG_VSOCK_ERR("send on closed conn\n");
        return false;
    }
    /* Honour peer's advertised RX buffer. Drop with a CREDIT_REQUEST
     * synth if the peer is back-pressured — caller should retry. */
    uint32_t in_flight = conn->tx_cnt - conn->peer_fwd_cnt;
    if (in_flight + len > conn->peer_buf_alloc) {
        LOG_VSOCK("peer buffer full (in_flight=%u alloc=%u), backpressuring\n",
                  in_flight, conn->peer_buf_alloc);
        return false;
    }

    struct virtio_vsock_hdr hdr;
    hdr_for_conn(conn, &hdr, VIRTIO_VSOCK_OP_RW, len, 0);
    bool ok = rx_push(vs, &hdr, payload, len);
    if (ok) conn->tx_cnt += len;
    return ok;
}

void virtio_vsock_close(struct virtio_vsock_device *vs,
                        struct virtio_vsock_conn *conn)
{
    if (!conn->in_use) return;
    if (conn->state == VSOCK_CONN_ESTABLISHED ||
        conn->state == VSOCK_CONN_CONNECTING) {
        struct virtio_vsock_hdr sd;
        hdr_for_conn(conn, &sd, VIRTIO_VSOCK_OP_SHUTDOWN, 0,
                     VIRTIO_VSOCK_SHUTDOWN_F_SEND |
                     VIRTIO_VSOCK_SHUTDOWN_F_RECV);
        rx_push(vs, &sd, NULL, 0);
    }
    if (vs->ops && vs->ops->on_close) {
        vs->ops->on_close(conn, vs->backend_user);
    }
    conn_free(conn);
}
