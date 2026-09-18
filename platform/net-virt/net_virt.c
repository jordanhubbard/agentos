/*
 * net_virt PD — network virtualizer (docs/TCB.md target: `net_virt`).
 *
 * The only network mux.  Owns no device frame and no IRQ.  Sits between the
 * guest-facing sDDF queues in the shared net frame (AOS_NET_SHMEM_VA, one
 * AOS_NET_CLIENT_STRIDE per guest client, produced/consumed by the VMM's
 * emulated virtio-net) and net_pd, the host NIC driver PD.
 *
 * Contract: include/contracts/net_virt_contract.h.
 *
 * Data path (per attached client):
 *   guest TX   VMM enqueues tx_active, signals KICK -> net_virt dequeues,
 *              copies the frame into net_pd's per-client slot, Calls
 *              NET_SVC_OP_RAW_SEND, recycles the buffer to tx_free.
 *   guest RX   net_pd NBSends NET_SVC_EVENT_RX_READY  ->  net_virt reserves
 *              an rx_free buffer, Calls NET_SVC_OP_RAW_RECV, copies the frame
 *              in, enqueues rx_active, signals the bound notification of the
 *              owning VMM, which pushes it into the guest virtq.
 *
 * When net_pd reports no host NIC (hw=0) the clients are wired into the
 * sDDF-shaped hub pump instead (one client: loopback; several: hub), which
 * is what the emulated-scope proof exercises on a board with no NIC.
 *
 * Guest and native queue notifications remain pending until received.
 * Driver RX_READY still uses endpoint events; before blocking we rescan
 * queues and probe net_pd once more (RAW_RECV polls the host ring).
 */

#include "agentos.h"
#include "sel4_ipc.h"
#include <stdio.h>
#include "system_desc.h"
#include "nameserver.h"
#include <contracts/net_virt_contract.h>
#include <contracts/virtualizer_authority.h>
#include <contracts/net-service/interface.h>
#include <platform/net_layout.h>
#include <platform/net_host_layout.h>
#include <platform/net_virt_pump.h>

_Static_assert(AOS_NET_SHMEM_VA == AGENTOS_NET_SHARED_VA,
               "guest net queues and net_pd slots share one frame");
_Static_assert(NET_SVC_SLOT_BASE == AOS_NET_DRIVER_SLOT_BASE &&
               NET_SVC_SHMEM_TOTAL == AOS_NET_SHMEM_SIZE,
               "network driver contract must match mapped frame layout");
_Static_assert(AOS_NET_QUEUE_CLIENTS * AOS_NET_CLIENT_STRIDE <= NET_SVC_SLOT_BASE,
               "guest net queues must not overlap net-service slots");
_Static_assert(sizeof(net_virt_attach_req_t) == 12u,
               "net_virt ATTACH request wire size");
_Static_assert(sizeof(net_virt_attach_reply_t) == 20u,
               "net_virt ATTACH reply wire size");

/* Root-provisioned log ring; the common debug fallback elsewhere. */
uintptr_t log_drain_rings_vaddr;

typedef struct {
    uint8_t               attached;
    uint8_t               retired;
    uint8_t               hw;          /* frames go to net_pd (else hub pump) */
    uint8_t               rx_pending;  /* probe net_pd for RX on next service */
    uint8_t               tx_marked;
    uint8_t               rx_marked;
    uint32_t              client_id;
    uint32_t              handle;      /* net_pd RAW handle */
    uint32_t              slot_off;    /* net_pd slot offset in the frame */
    seL4_CPtr             vmm_ep;      /* owning VMM listen EP (RX_READY) */
    uint32_t              rx_total;
    uint32_t              tx_total;
    aos_net_virt_client_t q;
} nv_client_t;

static nv_client_t     g_clients[AOS_NET_QUEUE_CLIENTS];
static aos_net_virt_t  g_hub;          /* loopback/hub pump for hw-less runs */
static int             g_hub_marked;

/* ── diagnostics ────────────────────────────────────────────────────────── */

static void nv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void nv_log(const char *fmt, ...)
{
    char buf[160];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    agentos_log_info("net_virt", buf);
}

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off] |
           ((uint32_t)p[off + 1u] << 8) |
           ((uint32_t)p[off + 2u] << 16) |
           ((uint32_t)p[off + 3u] << 24);
}

static void wr32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1u] = (uint8_t)(v >> 8);
    p[off + 2u] = (uint8_t)(v >> 16);
    p[off + 3u] = (uint8_t)(v >> 24);
}

static void nv_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) {
        dst[i] = src[i];
    }
}

static void nv_fence(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* ── net_pd RAW contract client ─────────────────────────────────────────── */

static int net_pd_call(uint32_t opcode, uint32_t arg0, uint32_t arg1,
                       sel4_msg_t *rep)
{
    sel4_msg_t req = {0};

    req.opcode = opcode;
    req.length = 8u;
    wr32(req.data, 0u, arg0);
    wr32(req.data, 4u, arg1);
    sel4_call((seL4_CPtr)PD_CNODE_SLOT_NET_PD_EP, &req, rep);
    return rep->opcode == SEL4_ERR_OK &&
           rd32(rep->data, 0u) == NET_SVC_RAW_OK;
}

static void register_with_nameserver(seL4_CPtr ns_ep)
{
    static const char name[] = "net_virt";
    sel4_msg_t req = {0}, rep = {0};

    if (!ns_ep) {
        return;
    }
    req.opcode = (uint32_t)OP_NS_REGISTER;
    wr32(req.data, 0u, 0u);
    wr32(req.data, 4u, 0u);
    wr32(req.data, 8u, 0u);
    wr32(req.data, 12u, 1u);
    for (uint32_t i = 0u; i < sizeof(name); i++) {
        req.data[16u + i] = (uint8_t)name[i];
    }
    req.length = 16u + (uint32_t)sizeof(name);
    sel4_call(ns_ep, &req, &rep);
}

/* ── events to the VMMs ─────────────────────────────────────────────────── */

static void nv_notify_vmm(const nv_client_t *c)
{
    if (c->client_id == AOS_NET_NATIVE_CLIENT) {
        seL4_Signal(PD_CNODE_SLOT_NET_NATIVE_NOTIFY);
        return;
    }
    if (c->vmm_ep != 0u) {
        seL4_Signal(c->vmm_ep);
    }
}

static seL4_CPtr vmm_ep_for_slot(uint32_t vmm_slot)
{
#ifdef AGENTOS_NATIVE_RUST_TEST
    if (vmm_slot == NET_VIRT_SLOT_NATIVE) return PD_CNODE_SLOT_NET_NATIVE_NOTIFY;
#endif
#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_X86_FIRMWARE_RESET)
    if (vmm_slot == NET_VIRT_VMM_SLOT_PRIMARY) {
        return (seL4_CPtr)PD_CNODE_SLOT_NET_PRIMARY_NOTIFY;
    }
#endif
#if defined(AGENTOS_GUEST_SECONDARY)
    if (vmm_slot == NET_VIRT_VMM_SLOT_SECONDARY) {
        return (seL4_CPtr)PD_CNODE_SLOT_NET_SECONDARY_NOTIFY;
    }
#endif
    (void)vmm_slot;
    return 0u;
}

/* ── TX: guest tx_active -> net_pd RAW_SEND ─────────────────────────────── */

static uint32_t nv_tx_to_net_pd(nv_client_t *c)
{
    uint32_t sent = 0u;
    aos_net_buff_desc_t buf;

    for (uint32_t attempt = 0u; attempt < c->q.capacity; attempt++) {
        if (aos_net_queue_dequeue(c->q.tx_active, c->q.capacity, &buf) != 0) break;
        uint32_t len = buf.len;
        uint8_t *dst = (uint8_t *)AGENTOS_NET_SHARED_VA + c->slot_off +
                       NET_SVC_TX_OFFSET;
        sel4_msg_t rep = {0};

        if (!aos_net_buffer_valid(buf.io_or_offset, len) ||
            len == 0u || len > NET_SVC_MAX_FRAME_BYTES) continue;
        {
            const uint8_t *src = c->q.tx_data + (uint32_t)buf.io_or_offset;
            nv_copy(dst, src, len);
            nv_fence();
            if (net_pd_call(NET_SVC_OP_RAW_SEND, c->handle, len, &rep)) {
                sent++;
                c->tx_total++;
                if (!c->tx_marked) {
                    c->tx_marked = 1u;
                    nv_log("TX accepted by net_pd");
                    nv_log("[net_pd] HOST_TX: QEMU bus.16 completion observed");
                }
            } else if (!c->tx_marked) {
                nv_log("TX rejected by net_pd rc=%u", (unsigned)rep.opcode);
            }
        }
        buf.len = 0u;
        (void)aos_net_queue_enqueue(c->q.tx_free, c->q.capacity, buf);
    }
    return sent;
}

/* ── RX: net_pd RAW_RECV -> guest rx_active ─────────────────────────────── */

static uint32_t nv_rx_from_net_pd(nv_client_t *c)
{
    uint32_t received = 0u;

    /* Normal state: no backpressure, the VMM need not kick on RX recycle. */
    c->q.rx_free->consumer_signalled = 1u;
    nv_fence();

    for (uint32_t attempt = 0u; attempt < AOS_NET_CAPACITY; attempt++) {
        aos_net_buff_desc_t buf;
        sel4_msg_t rep = {0};
        uint32_t len, off;

        /*
         * Reserve the destination before RAW_RECV transfers ownership of a
         * frame out of net_pd's ring.  Consuming first loses a frame whenever
         * the guest has temporarily exhausted its RX descriptors.
         */
        if (aos_net_queue_dequeue(c->q.rx_free, c->q.capacity, &buf) != 0) {
            /* Backpressured: ask the VMM to kick when it recycles buffers. */
            c->q.rx_free->consumer_signalled = 0u;
            nv_fence();
            break;
        }
        if (!aos_net_buffer_valid(buf.io_or_offset, 0u)) continue;
        if (!net_pd_call(NET_SVC_OP_RAW_RECV, c->handle,
                         NET_SVC_MAX_FRAME_BYTES, &rep) ||
            rep.length < 12u) {
            (void)aos_net_queue_enqueue(c->q.rx_free, c->q.capacity, buf);
            c->rx_pending = 0u;
            break;
        }
        len = rd32(rep.data, 4u);
        off = rd32(rep.data, 8u);
        if (len == 0u) {
            (void)aos_net_queue_enqueue(c->q.rx_free, c->q.capacity, buf);
            c->rx_pending = 0u;
            break;
        }
        if (len > NET_SVC_MAX_FRAME_BYTES || len > AOS_NET_BUFFER_SIZE ||
            off < NET_SVC_SLOT_BASE || off > AGENTOS_NET_SHARED_SIZE ||
            len > AGENTOS_NET_SHARED_SIZE - off ||
            !aos_net_buffer_valid(buf.io_or_offset, len)) {
            (void)aos_net_queue_enqueue(c->q.rx_free, c->q.capacity, buf);
            nv_log("RX bounds invalid from net_pd");
            break;
        }
        nv_fence();
        nv_copy(c->q.rx_data + (uint32_t)buf.io_or_offset,
                (const uint8_t *)AGENTOS_NET_SHARED_VA + off, len);
        buf.len = (uint16_t)len;
        if (aos_net_queue_enqueue(c->q.rx_active, c->q.capacity, buf) != 0) {
            buf.len = 0u;
            (void)aos_net_queue_enqueue(c->q.rx_free, c->q.capacity, buf);
            break;
        }
        received++;
        c->rx_total++;
        if (!c->rx_marked) {
            c->rx_marked = 1u;
            nv_log("RX delivered from net_pd");
        }
    }
    return received;
}

/* ── service round ──────────────────────────────────────────────────────── */

static int nv_rescan_needed(void)
{
    for (uint32_t i = 0u; i < AOS_NET_QUEUE_CLIENTS; i++) {
        const nv_client_t *c = &g_clients[i];

        if (!c->attached) {
            continue;
        }
        if (aos_net_queue_length(c->q.tx_active) != 0u) {
            return 1;
        }
        if (c->hw && c->rx_pending &&
            aos_net_queue_length(c->q.rx_free) != 0u) {
            return 1;
        }
    }
    return 0;
}

static void nv_service(void)
{
    for (uint32_t pass = 0u; pass < 4u; pass++) {
        uint32_t deliver[AOS_NET_QUEUE_CLIENTS] = {0};
        int any_hub = 0;

        /* Draining: kicks are redundant until we ask for them again. */
        for (uint32_t i = 0u; i < AOS_NET_QUEUE_CLIENTS; i++) {
            if (g_clients[i].attached) {
                g_clients[i].q.tx_active->consumer_signalled = 1u;
            }
        }
        nv_fence();

        for (uint32_t i = 0u; i < AOS_NET_QUEUE_CLIENTS; i++) {
            nv_client_t *c = &g_clients[i];

            if (!c->attached) {
                continue;
            }
            if (c->hw) {
                (void)nv_tx_to_net_pd(c);
                /*
                 * Always probe once: a RX_READY from net_pd that landed
                 * while we were busy is dropped, and RAW_RECV polls the host
                 * ring, so this closes that window before we block.
                 */
                c->rx_pending = 1u;
                deliver[i] = nv_rx_from_net_pd(c);
            } else {
                any_hub = 1;
            }
        }
        if (any_hub) {
            uint32_t moved = aos_net_virt_pump(&g_hub);

            if (moved > 0u && !g_hub_marked) {
                g_hub_marked = 1;
                nv_log("pumped %u frame(s) TX->RX (hub/loopback: net_pd reports no host NIC)", (unsigned)moved);
            }
            for (uint32_t i = 0u; i < AOS_NET_QUEUE_CLIENTS; i++) {
                nv_client_t *c = &g_clients[i];

                if (c->attached && !c->hw &&
                    aos_net_queue_length(c->q.rx_active) != 0u) {
                    deliver[i] = 1u;
                }
            }
        }

        for (uint32_t i = 0u; i < AOS_NET_QUEUE_CLIENTS; i++) {
            if (deliver[i] != 0u) {
                nv_notify_vmm(&g_clients[i]);
            }
        }

        /* Ask for kicks again, then rescan so nothing enqueued meanwhile
         * waits for the next kick. */
        for (uint32_t i = 0u; i < AOS_NET_QUEUE_CLIENTS; i++) {
            if (g_clients[i].attached) {
                g_clients[i].q.tx_active->consumer_signalled = 0u;
            }
        }
        nv_fence();
        if (!nv_rescan_needed()) {
            break;
        }
    }
}

/* ── ATTACH ─────────────────────────────────────────────────────────────── */

static void handle_attach(uint64_t badge, const sel4_msg_t *req, sel4_msg_t *rep)
{
    uint32_t version = rd32(req->data, 0u);
    uint32_t client_id = rd32(req->data, 4u);
    uint32_t vmm_slot = rd32(req->data, 8u);
    uint32_t status = NET_VIRT_OK;
    uint32_t hw_state = NET_VIRT_HW_NONE;
    uint8_t mac[6] = {0};
    nv_client_t *c = NULL;
    seL4_CPtr vmm_ep = vmm_ep_for_slot(vmm_slot);

    if (version != NET_VIRT_CONTRACT_VERSION || req->length < 12u) {
        status = NET_VIRT_ERR_VERSION;
    } else if (!virt_net_authorized(badge, client_id, vmm_slot) ||
               client_id >= AOS_NET_QUEUE_CLIENTS || vmm_ep == 0u) {
        status = NET_VIRT_ERR_BAD_CLIENT;
    } else if (g_clients[client_id].attached || g_clients[client_id].retired) {
        status = NET_VIRT_ERR_BUSY;
    }

    if (status == NET_VIRT_OK) {
        sel4_msg_t nrep = {0};

        c = &g_clients[client_id];
        c->client_id = client_id;
        c->vmm_ep = vmm_ep;
        c->hw = 0u;
        /* The VMM initialised the buffers before attaching; only bind. */
        aos_net_client_bind((uint8_t *)AOS_NET_SHMEM_VA, client_id, &c->q);

        if (net_pd_call(NET_SVC_OP_RAW_OPEN, client_id, 0u, &nrep) &&
            nrep.length >= 18u) {
            uint32_t slot = rd32(nrep.data, 8u);

            if (slot >= NET_SVC_SLOT_BASE &&
                slot + NET_SVC_SLOT_SIZE <= AGENTOS_NET_SHARED_SIZE) {
                c->handle = rd32(nrep.data, 4u);
                c->slot_off = slot;
                for (uint32_t i = 0u; i < 6u; i++) {
                    mac[i] = nrep.data[12u + i];
                }
                nrep = (sel4_msg_t){0};
                if (net_pd_call(NET_SVC_OP_RAW_STATUS, c->handle, 0u, &nrep) &&
                    rd32(nrep.data, 4u) != 0u) {
                    c->hw = 1u;
                    hw_state = NET_VIRT_HW_NET_PD;
                }
            } else {
                nv_log("net_pd returned an invalid slot");
            }
        } else {
            nv_log("net_pd RAW_OPEN failed");
        }

        if (!c->hw) {
            (void)aos_net_virt_add_client(&g_hub, &c->q);
        }
        /* Protocol state: want kicks for TX; no RX backpressure yet. */
        c->q.tx_active->consumer_signalled = 0u;
        c->q.rx_free->consumer_signalled = 1u;
        nv_fence();
        c->attached = 1u;

        nv_log("ATTACH client=%u vmm_slot=%u net_pd contract v%u hw=%u%s",
               (unsigned)client_id, (unsigned)vmm_slot,
               (unsigned)NET_SVC_INTERFACE_VERSION, (unsigned)c->hw,
               c->hw ? "" : " (hub/loopback pump)");
        if (c->hw) {
            nv_log("[net_pd] HOST_READY: virtio-net bus.16");
        }
    } else {
        nv_log("ATTACH rejected status=%u", (unsigned)status);
    }

    wr32(rep->data, 0u, status);
    wr32(rep->data, 4u, NET_VIRT_CONTRACT_VERSION);
    wr32(rep->data, 8u, hw_state);
    for (uint32_t i = 0u; i < 6u; i++) {
        rep->data[12u + i] = mac[i];
    }
    rep->length = (uint32_t)sizeof(net_virt_attach_reply_t);
    rep->opcode = SEL4_ERR_OK;
}

static void handle_detach(uint64_t badge, const sel4_msg_t *req, sel4_msg_t *rep)
{
    uint32_t client_id = rd32(req->data, 4u);
    uint32_t slot = rd32(req->data, 8u);
    uint32_t status = NET_VIRT_OK;
    if (req->length != sizeof(net_virt_attach_req_t) ||
        rd32(req->data, 0u) != NET_VIRT_CONTRACT_VERSION) {
        status = NET_VIRT_ERR_VERSION;
    } else if (!virt_net_authorized(badge, client_id, slot) ||
               client_id >= AOS_NET_QUEUE_CLIENTS || !vmm_ep_for_slot(slot)) {
        status = NET_VIRT_ERR_BAD_CLIENT;
    } else {
        nv_client_t *c = &g_clients[client_id];
        /* This PD serializes control and pumping. RAW_SEND/RECV are
         * synchronous copies into the driver's separate transfer page, so
         * none can retain a client-page reference after returning here. */
        if (c->attached && !c->hw && aos_net_virt_remove_client(&g_hub, &c->q)) {
            status = NET_VIRT_ERR_UNAVAILABLE;
        } else {
            *c = (nv_client_t){ .retired = 1u };
            nv_log("DETACH client=%u queues released", (unsigned)client_id);
        }
    }
    wr32(rep->data, 0u, status);
    wr32(rep->data, 4u, NET_VIRT_CONTRACT_VERSION);
    rep->length = sizeof(net_virt_attach_reply_t);
    rep->opcode = SEL4_ERR_OK;
}

/* ── main loop ──────────────────────────────────────────────────────────── */

static void net_virt_run(seL4_CPtr ep)
{
    for (;;) {
        seL4_Word badge = 0u;
        sel4_msg_t req = {0};
        sel4_msg_t rep = {0};
#ifdef CONFIG_KERNEL_MCS
        seL4_MessageInfo_t info = seL4_Recv(ep, &badge, AGENTOS_IPC_REPLY_CAP);
#else
        seL4_MessageInfo_t info = seL4_Recv(ep, &badge);
#endif
        seL4_Word label = seL4_MessageInfo_get_label(info);
        if (badge && !(badge & ~(NET_VIRT_NATIVE_WAKE_BADGE | NET_VIRT_GUEST_WAKE_MASK))) {
            nv_service();
            continue;
        }

        if (label == NET_VIRT_OP_ATTACH || label == NET_VIRT_OP_DETACH) {
            _sel4_mrs_to_msg(&req);
            if (label == NET_VIRT_OP_ATTACH) handle_attach(badge, &req, &rep);
            else handle_detach(badge, &req, &rep);
            _sel4_msg_to_mrs(&rep);
            seL4_MessageInfo_t reply = seL4_MessageInfo_new(
                (seL4_Word)rep.opcode, 0u, 0u, (seL4_Word)_SEL4_MR_COUNT);
#ifdef CONFIG_KERNEL_MCS
            seL4_Send(AGENTOS_IPC_REPLY_CAP, reply);
#else
            seL4_Reply(reply);
#endif
            continue;
        }
        if (label == NET_VIRT_EVENT_KICK || label == NET_SVC_EVENT_RX_READY) {
            nv_service();
            continue;
        }
        /* Anything else (stray fault labels, unknown events) is ignored. */
    }
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    agentos_log_boot("net_virt");
    aos_net_virt_reset(&g_hub);
    register_with_nameserver(ns_ep);
    nv_log("READY: contract v6, isolated capability-bound clients, no device caps");
    net_virt_run(my_ep);
}
