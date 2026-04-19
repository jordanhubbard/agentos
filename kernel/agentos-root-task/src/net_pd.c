/*
 * net_pd.c — agentOS Generic Network Protection Domain
 *
 * OS-neutral IPC network interface service. Provides OPEN/CLOSE/SEND/RECV/STATUS/
 * CONFIGURE/FILTER_ADD/FILTER_REMOVE over seL4 IPC with zero-copy shared memory.
 * One handle per exclusive interface claim; up to NET_MAX_HANDLES simultaneous clients.
 *
 * IPC Protocol (caller -> net_pd, channel CH_NET_PD):
 *   MSG_NET_OPEN          (0x1010) — claim interface → handle
 *   MSG_NET_CLOSE         (0x1011) — release handle
 *   MSG_NET_SEND          (0x1012) — MR1=handle MR2=offset MR3=len → ok
 *   MSG_NET_RECV          (0x1013) — MR1=handle MR2=offset MR3=max → MR1=len
 *   MSG_NET_STATUS        (0x1014) — MR1=handle → MR1=link MR2=rx MR3=tx
 *   MSG_NET_CONFIGURE     (0x1015) — MR1=handle MR2=flags MR3=mtu → ok
 *   MSG_NET_FILTER_ADD    (0x1016) — MR1=handle MR2=rule_id → ok
 *   MSG_NET_FILTER_REMOVE (0x1017) — MR1=handle MR2=rule_id → ok
 *
 * Hardware: virtio-net MMIO or physical NIC via device capability.
 *           RX ring drain and TX descriptor management are Phase 2.5.
 *
 * Priority: 215
 */
#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/net_contract.h"

/* ── State ───────────────────────────────────────────────────────────────── */
static bool     s_handle_open[NET_MAX_HANDLES];
static uint32_t s_iface[NET_MAX_HANDLES];     /* physical interface index */
static uint32_t s_rx_packets[NET_MAX_HANDLES];
static uint32_t s_tx_packets[NET_MAX_HANDLES];
static uint32_t s_mtu[NET_MAX_HANDLES];
static uint32_t s_flags[NET_MAX_HANDLES];
static uint32_t s_link_up[NET_MAX_HANDLES];

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void drain_rx_ring(uint32_t handle)
{
    /*
     * TODO Phase 2.5: walk virtio-net RX descriptor ring for s_iface[handle],
     * copy available frames into net_shmem, advance used ring index.
     * For now this is a no-op placeholder.
     */
    (void)handle;
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    for (uint32_t i = 0; i < NET_MAX_HANDLES; i++) {
        s_handle_open[i] = false;
        s_iface[i]       = 0;
        s_rx_packets[i]  = 0;
        s_tx_packets[i]  = 0;
        s_mtu[i]         = 1500;
        s_flags[i]       = 0;
        s_link_up[i]     = 0;
    }
    microkit_dbg_puts("[net_pd] ready\n");
}

void notified(microkit_channel ch)
{
    /*
     * A notification on any channel means a receive ring may have new frames.
     * Drain all open handles' RX rings so data is available for the next RECV call.
     */
    (void)ch;
    for (uint32_t i = 0; i < NET_MAX_HANDLES; i++) {
        if (s_handle_open[i]) drain_rx_ring(i);
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch; (void)msginfo;
    uint32_t op     = (uint32_t)microkit_mr_get(0);
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    switch (op) {
    case MSG_NET_OPEN: {
        uint32_t iface = handle; /* MR1 carries iface_index on OPEN */
        uint32_t found = NET_MAX_HANDLES;
        for (uint32_t i = 0; i < NET_MAX_HANDLES; i++) {
            if (!s_handle_open[i]) { found = i; break; }
        }
        if (found == NET_MAX_HANDLES) {
            microkit_mr_set(0, NET_ERR_NO_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        s_handle_open[found] = true;
        s_iface[found]       = iface;
        s_link_up[found]     = 1; /* TODO Phase 2.5: query real link state */
        microkit_mr_set(0, NET_OK);
        microkit_mr_set(1, found);
        return microkit_msginfo_new(0, 2);
    }
    case MSG_NET_CLOSE:
        if (handle >= NET_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, NET_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        s_handle_open[handle] = false;
        microkit_mr_set(0, NET_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_NET_SEND:
        if (handle >= NET_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, NET_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        /* TODO Phase 2.5: enqueue descriptor into virtio-net TX ring */
        s_tx_packets[handle]++;
        microkit_mr_set(0, NET_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_NET_RECV:
        if (handle >= NET_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, NET_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        /* TODO Phase 2.5: copy from RX ring buffer into caller shmem */
        drain_rx_ring(handle);
        microkit_mr_set(0, NET_OK);
        microkit_mr_set(1, 0); /* no data yet */
        return microkit_msginfo_new(0, 2);
    case MSG_NET_STATUS:
        if (handle >= NET_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, NET_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        microkit_mr_set(0, NET_OK);
        microkit_mr_set(1, s_link_up[handle]);
        microkit_mr_set(2, s_rx_packets[handle]);
        microkit_mr_set(3, s_tx_packets[handle]);
        return microkit_msginfo_new(0, 4);
    case MSG_NET_CONFIGURE:
        if (handle >= NET_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, NET_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        s_flags[handle] = (uint32_t)microkit_mr_get(2);
        s_mtu[handle]   = (uint32_t)microkit_mr_get(3);
        microkit_mr_set(0, NET_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_NET_FILTER_ADD:
        if (handle >= NET_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, NET_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        /* TODO Phase 2.5: install filter rule into hardware or software filter table */
        microkit_mr_set(0, NET_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_NET_FILTER_REMOVE:
        if (handle >= NET_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, NET_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        /* TODO Phase 2.5: remove filter rule */
        microkit_mr_set(0, NET_OK);
        return microkit_msginfo_new(0, 1);
    default:
        microkit_dbg_puts("[net_pd] unknown op\n");
        microkit_mr_set(0, NET_ERR_NOT_IMPL);
        return microkit_msginfo_new(0, 1);
    }
}
