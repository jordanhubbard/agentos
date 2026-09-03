/*
 * libvmm/virtio/vsock.h — virtio-vsock device backend
 *
 * Implements the host side of a virtio-vsock transport so that a Linux
 * guest's stock vsock driver (CONFIG_VHOST_VSOCK / virtio-vsock) can
 * exchange AF_VSOCK packets with a hypervisor-side service. The guest
 * sees a normal virtio-vsock device on the MMIO/PCI bus; this backend
 * forwards packets to and from a cooperating PD via the callback
 * interface declared at the bottom of this file.
 *
 * Spec reference: Virtio 1.2, "5.10 Socket Device".
 *
 * Copyright 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sel4/sel4.h>
#include <libvmm/virtio/virtio.h>

/* virtio-vsock has three virtqueues: RX (host->guest), TX (guest->host),
 * and an event queue used by the host to inform the guest of transport
 * resets (e.g. CID reassignment). The event queue is rarely populated
 * by sane backends; we keep a slot for it but don't push events. */
#define VIRTIO_VSOCK_QUEUE_RX     0
#define VIRTIO_VSOCK_QUEUE_TX     1
#define VIRTIO_VSOCK_QUEUE_EVENT  2
#define VIRTIO_VSOCK_NUM_VIRTQ    3

/* Feature bits (virtio-spec 5.10.3). We don't negotiate any optional
 * features in v1 — the only bit we need is VIRTIO_F_VERSION_1, which
 * is offered by libvmm's mmio core in the upper 32 bits. */
#define VIRTIO_VSOCK_F_STREAM     0  /* SOCK_STREAM supported (mandatory if any feature) */
#define VIRTIO_VSOCK_F_SEQPACKET  1  /* SOCK_SEQPACKET supported */
#define VIRTIO_VSOCK_F_DGRAM      3  /* SOCK_DGRAM supported (post-1.2 erratum) */

/* The virtio-vsock device config space is a single 64-bit field: the
 * host-assigned CID (Context ID) for the guest. Host CID is fixed at
 * VMADDR_CID_HOST=2 by AF_VSOCK convention; guest CIDs are >= 3. */
struct virtio_vsock_config {
    uint64_t guest_cid;
} __attribute__((packed));

/* Packet header that prefixes every payload on every queue
 * (virtio-spec 5.10.6). Little-endian on the wire. */
struct virtio_vsock_hdr {
    uint64_t src_cid;
    uint64_t dst_cid;
    uint32_t src_port;
    uint32_t dst_port;
    uint32_t len;        /* payload length, not including this header */
    uint16_t type;       /* VIRTIO_VSOCK_TYPE_* */
    uint16_t op;         /* VIRTIO_VSOCK_OP_* */
    uint32_t flags;      /* op-specific; see VIRTIO_VSOCK_SHUTDOWN_F_* */
    uint32_t buf_alloc;  /* receiver's buffer size, for credit accounting */
    uint32_t fwd_cnt;    /* bytes the receiver has forwarded to its app */
} __attribute__((packed));

/* Socket type. We support STREAM in v1 (matches Linux's primary use). */
#define VIRTIO_VSOCK_TYPE_STREAM     1
#define VIRTIO_VSOCK_TYPE_SEQPACKET  2
#define VIRTIO_VSOCK_TYPE_DGRAM      3

/* Operations (virtio-spec 5.10.6.1). */
#define VIRTIO_VSOCK_OP_INVALID         0
#define VIRTIO_VSOCK_OP_REQUEST         1  /* connect: client -> server */
#define VIRTIO_VSOCK_OP_RESPONSE        2  /* accept:  server -> client */
#define VIRTIO_VSOCK_OP_RST             3  /* reset connection (any side) */
#define VIRTIO_VSOCK_OP_SHUTDOWN        4  /* close one or both directions */
#define VIRTIO_VSOCK_OP_RW              5  /* data transfer */
#define VIRTIO_VSOCK_OP_CREDIT_UPDATE   6  /* unsolicited credit grant */
#define VIRTIO_VSOCK_OP_CREDIT_REQUEST  7  /* request credit grant */

/* SHUTDOWN flag bits: which directions to close. */
#define VIRTIO_VSOCK_SHUTDOWN_F_RECV  (1u << 0)
#define VIRTIO_VSOCK_SHUTDOWN_F_SEND  (1u << 1)

/* Well-known CIDs (Linux uapi/linux/vm_sockets.h). */
#define VIRTIO_VSOCK_CID_HYPERVISOR  0
#define VIRTIO_VSOCK_CID_LOCAL       1  /* loopback within a guest */
#define VIRTIO_VSOCK_CID_HOST        2
/* Guest CIDs are assigned by the hypervisor; valid range is 3..0xFFFFFFFE. */

/* Maximum number of simultaneously open connections this backend tracks.
 * Each entry is ~64 bytes; 64 entries = 4KB, comfortable for the
 * exec-spawn use case (a handful of in-flight subprocess launches). */
#define VIRTIO_VSOCK_MAX_CONNS  64

/* Per-connection state. Indexed by a 4-tuple lookup on incoming packets;
 * we don't bother with a hash because MAX_CONNS is small. */
struct virtio_vsock_conn {
    bool     in_use;
    uint64_t local_cid;     /* always VIRTIO_VSOCK_CID_HOST */
    uint64_t peer_cid;      /* the guest's CID */
    uint32_t local_port;
    uint32_t peer_port;
    uint16_t type;          /* VIRTIO_VSOCK_TYPE_STREAM */
    enum {
        VSOCK_CONN_CLOSED = 0,
        VSOCK_CONN_LISTEN,         /* host advertised a listener */
        VSOCK_CONN_CONNECTING,     /* guest sent REQUEST, awaiting backend accept */
        VSOCK_CONN_ESTABLISHED,
        VSOCK_CONN_CLOSING_LOCAL,  /* host called shutdown() */
        VSOCK_CONN_CLOSING_PEER,   /* guest sent SHUTDOWN */
    } state;
    /* Credit accounting (virtio-spec 5.10.6.3). The backend updates
     * peer_buf_alloc / peer_fwd_cnt as packets arrive; tx_cnt tracks
     * how many bytes we've sent so we know when to stall. */
    uint32_t peer_buf_alloc;
    uint32_t peer_fwd_cnt;
    uint32_t tx_cnt;
    uint32_t rx_cnt;
    /* Opaque token the backend stashes here when it accepts a connection;
     * forwarded back on every packet so the backend can dispatch without
     * its own connection table. */
    void    *backend_cookie;
};

/* Callback table: the hypervisor-side service (e.g. cluster_spawn_pd
 * proxy living inside linux_vmm) registers these. They run synchronously
 * on the libvmm thread that's processing the TX virtqueue, so they
 * must not block.
 *
 * For host->guest pushes, the backend instead calls virtio_vsock_inject()
 * (declared below).
 */
typedef struct virtio_vsock_backend_ops {
    /* Guest issued OP_REQUEST to (cid=HOST, port=dst_port). The backend
     * decides whether to accept; on accept, it should populate
     * conn->backend_cookie and return true. The vsock device will
     * synthesise the OP_RESPONSE. On reject, return false; the vsock
     * device synthesises OP_RST. */
    bool (*on_connect)(struct virtio_vsock_conn *conn, void *user);

    /* Guest sent OP_RW. payload is host-virtual, valid only for the
     * duration of the call. The backend must copy if it needs to
     * retain. Return true on success; false triggers OP_RST. */
    bool (*on_recv)(struct virtio_vsock_conn *conn,
                    const void *payload, uint32_t len, void *user);

    /* Guest closed (OP_SHUTDOWN with both flags, or OP_RST). The backend
     * should release its cookie. on_close is also invoked when the host
     * calls virtio_vsock_close(). */
    void (*on_close)(struct virtio_vsock_conn *conn, void *user);

    /* Optional. Fires after the device has synthesised OP_RESPONSE for
     * an accepted connection — i.e. once the connection is fully
     * established from the guest's point of view. The backend may use
     * this to send the first host→guest packet (e.g. the first work
     * item). May be NULL. Runs in the same TX-virtqueue processing
     * context as on_connect. */
    void (*on_accepted)(struct virtio_vsock_conn *conn, void *user);
} virtio_vsock_backend_ops_t;

/* Top-level device state. Allocated and owned by the linux_vmm
 * instantiation site; passed by pointer everywhere. */
struct virtio_vsock_device {
    struct virtio_device          virtio_device;
    struct virtio_queue_handler   vqs[VIRTIO_VSOCK_NUM_VIRTQ];
    struct virtio_vsock_config    config;
    struct virtio_vsock_conn      conns[VIRTIO_VSOCK_MAX_CONNS];
    const virtio_vsock_backend_ops_t *ops;
    void                         *backend_user;
};

/* Initialise + register a virtio-vsock device on the MMIO transport.
 *
 *   region_base / region_size  MMIO window in the guest's PA space
 *   virq                       SPI to inject on used-ring notifications
 *   guest_cid                  the CID this guest will see in its config
 *                              space (must be >= 3 and != 0xFFFFFFFF)
 *   ops, user                  backend callbacks + opaque user pointer
 *
 * Returns false if region_base is already taken or virq is invalid.
 */
bool virtio_mmio_vsock_init(struct virtio_vsock_device *vsock,
                            uintptr_t region_base,
                            uintptr_t region_size,
                            size_t virq,
                            uint64_t guest_cid,
                            const virtio_vsock_backend_ops_t *ops,
                            void *user);

/* Push a payload to the guest on an established connection. Frames it
 * with a virtio_vsock_hdr (op=RW), copies into the next available RX
 * descriptor, and asserts the device IRQ. Returns false if the RX queue
 * is full (the backend should retry; this is normal flow control). */
bool virtio_vsock_send(struct virtio_vsock_device *vsock,
                       struct virtio_vsock_conn *conn,
                       const void *payload, uint32_t len);

/* Host-initiated close: synthesises an OP_SHUTDOWN(SEND|RECV) to the
 * guest, transitions to CLOSING_LOCAL, and invokes on_close after the
 * guest acknowledges with OP_RST. */
void virtio_vsock_close(struct virtio_vsock_device *vsock,
                        struct virtio_vsock_conn *conn);
