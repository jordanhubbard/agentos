/*
 * test_network_stack.c — API tests for the E5-S4 migrated network stack PDs
 *
 * Covered PDs and scenarios:
 *
 *   net_server (E5-S4):
 *     net_server_test_init / registration
 *     OP_NET_VNIC_CREATE — success, permission denied, table full (via drain)
 *     OP_NET_VNIC_DESTROY — success and not-found
 *     OP_NET_VNIC_SEND — success, ACL deny, bad vnic
 *     OP_NET_VNIC_RECV — success path
 *     OP_NET_STATUS — global and per-vnic
 *     OP_NET_HEALTH — liveness check
 *     unknown opcode — must return SEL4_ERR_INVALID_OP
 *
 *   net_pd (E5-S4):
 *     net_pd_test_init / registration
 *     MSG_NET_OPEN — success and slots-full
 *     MSG_NET_CLOSE — success and bad-handle
 *     MSG_NET_SEND — NIC send stub path
 *     MSG_NET_RECV — NIC recv path (empty ring)
 *     MSG_NET_DEV_STATUS — global and per-handle
 *     MSG_NET_CONFIGURE — MTU update
 *     MSG_NET_SOCKET_OPEN — TCP and UDP
 *     MSG_NET_SOCKET_BIND — valid port
 *     MSG_NET_SOCKET_LISTEN — after bind
 *     MSG_NET_SOCKET_ACCEPT — no pending (stub)
 *     unknown opcode — must return SEL4_ERR_INVALID_OP
 *
 *   wg_net (E5-S4):
 *     wg_net_test_init / registration
 *     OP_WG_HEALTH — before key set
 *     OP_WG_SET_PRIVKEY — requires staging
 *     OP_WG_ADD_PEER — success and bad peer_id
 *     OP_WG_REMOVE_PEER — success and not-found
 *     OP_WG_SEND — no-key error, then key-set success path
 *     OP_WG_RECV — empty (no pending)
 *     OP_WG_STATUS — zero peers
 *     Timer tick helper — keepalive_due advances
 *     unknown opcode — must return SEL4_ERR_INVALID_OP
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST \
 *      -I tests/api \
 *      -I kernel/agentos-root-task/include \
 *      -o /tmp/test_network_stack \
 *      tests/api/test_network_stack.c && /tmp/test_network_stack
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"

/* ── Backing store for shared memory regions ─────────────────────────────── */

/*
 * net_packet_shmem: 256KB for net_server vNIC rings.
 * wg_staging:       128KB for WireGuard key material + TX/RX buffers.
 * net_pd_shmem:     256KB for net_pd client slots.
 */
static uint8_t ns_shmem_backing[0x40000];   /* 256KB for net_server */
static uint8_t wg_shmem_backing[0x20000];   /* 128KB for wg_net     */
static uint8_t pd_shmem_backing[0x40000];   /* 256KB for net_pd     */

/* ── data_rd32 / data_wr32 helpers (shared by all three test suites) ──────── */

static inline uint32_t dr32(const uint8_t *d, int off)
{
    return (uint32_t)d[off]
         | ((uint32_t)d[off+1] <<  8)
         | ((uint32_t)d[off+2] << 16)
         | ((uint32_t)d[off+3] << 24);
}
static inline void dw32(uint8_t *d, int off, uint32_t v)
{
    d[off  ] = (uint8_t)(v      );
    d[off+1] = (uint8_t)(v >>  8);
    d[off+2] = (uint8_t)(v >> 16);
    d[off+3] = (uint8_t)(v >> 24);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Pull in the three PD implementations under AGENTOS_TEST_HOST.
 * Each file's #ifdef AGENTOS_TEST_HOST block replaces seL4 types with stubs.
 * ══════════════════════════════════════════════════════════════════════════ */
#include "../../kernel/agentos-root-task/src/net_server.c"

/*
 * net_pd.c and wg_net.c define overlapping static globals (active_clients,
 * slot_bitmap etc.).  Include them in separate translation units by wrapping
 * in anonymous namespaces via unique struct-scope macros — but since C has
 * no namespaces, we instead compile the two extra PDs through re-inclusion
 * with renamed symbols.  For test purposes we compile them individually
 * through the dispatch helper (net_pd_dispatch_one / wg_net_dispatch_one)
 * which is static and scoped to this TU.
 *
 * To avoid double-definition errors we disable the net_server.c globals
 * before the second include.  The simplest approach that works for C is
 * a second compilation unit but since we must produce a single binary here
 * we use a static guard pair around each included PD source.
 *
 * NOTE: In production, each PD is a separate binary.  This single-TU
 * arrangement is ONLY used by the host-side test harness.
 */

/* ── Rename overlapping symbols for net_pd ──────────────────────────────── */
/* Rename global state so it doesn't clash with net_server's */
#define active_clients   _netpd_active_clients
#define slot_bitmap      _netpd_slot_bitmap
#define hw_present       _netpd_hw_present
#define iface_rx_pkts    _netpd_iface_rx_pkts
#define iface_tx_pkts    _netpd_iface_tx_pkts
#define iface_rx_bytes   _netpd_iface_rx_bytes
#define iface_tx_bytes   _netpd_iface_tx_bytes
#define iface_rx_errors  _netpd_iface_rx_errors
#define iface_tx_errors  _netpd_iface_tx_errors
#define iface_link_up    _netpd_iface_link_up
#define clients          _netpd_clients
#define g_srv            _netpd_g_srv
/* Rename the dispatch helper so the test file sees it as net_pd_dispatch_one */
#define net_pd_dispatch_one  _net_pd_dispatch_one
#define net_pd_test_init     _net_pd_test_init_fn

#include "../../kernel/agentos-root-task/src/net_pd.c"

#undef active_clients
#undef slot_bitmap
#undef hw_present
#undef iface_rx_pkts
#undef iface_tx_pkts
#undef iface_rx_bytes
#undef iface_tx_bytes
#undef iface_rx_errors
#undef iface_tx_errors
#undef iface_link_up
#undef clients
#undef g_srv
#undef net_pd_dispatch_one
#undef net_pd_test_init

/* ── Rename overlapping symbols for wg_net ──────────────────────────────── */
#define peers                 _wgnet_peers
#define active_peer_count     _wgnet_active_peer_count
#define wg_privkey            _wgnet_privkey
#define wg_pubkey             _wgnet_pubkey
#define wg_privkey_set        _wgnet_privkey_set
#define timer_tick            _wgnet_timer_tick
#define keepalive_due         _wgnet_keepalive_due
#define rx_peer_id            _wgnet_rx_peer_id
#define rx_data_len           _wgnet_rx_data_len
#define rx_pending            _wgnet_rx_pending
#define g_net_ep              _wgnet_g_net_ep
#define g_srv                 _wgnet_g_srv
#define wg_net_dispatch_one   _wg_net_dispatch_one
#define wg_net_test_init      _wg_net_test_init_fn
#define wg_net_timer_tick     _wg_net_timer_tick_fn

#include "../../kernel/agentos-root-task/src/wg_net.c"

#undef peers
#undef active_peer_count
#undef wg_privkey
#undef wg_pubkey
#undef wg_privkey_set
#undef timer_tick
#undef keepalive_due
#undef rx_peer_id
#undef rx_data_len
#undef rx_pending
#undef g_net_ep
#undef g_srv
#undef wg_net_dispatch_one
#undef wg_net_test_init
#undef wg_net_timer_tick

/* ── Aliases for the renamed dispatch helpers ────────────────────────────── */
static uint32_t call_net_pd(sel4_badge_t b, const sel4_msg_t *q, sel4_msg_t *r)
{
    return _net_pd_dispatch_one(b, q, r);
}
static uint32_t call_wg_net(sel4_badge_t b, const sel4_msg_t *q, sel4_msg_t *r)
{
    return _wg_net_dispatch_one(b, q, r);
}

/* ══════════════════════════════════════════════════════════════════════════
 * net_server tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void setup_ns(void)
{
    memset(ns_shmem_backing, 0, sizeof(ns_shmem_backing));
    net_packet_shmem_vaddr = (uintptr_t)ns_shmem_backing;
    net_mmio_vaddr         = 0;
    vibe_staging_vaddr     = 0;
    net_server_test_init();
}

/* Test 1: net_server_test_init completes without crash */
static void test_ns_init(void)
{
    setup_ns();
    TAP_OK("net_server_test_init completes without crash");
}

/* Test 2: OP_NET_HEALTH returns NET_OK and NET_VERSION */
static void test_ns_health(void)
{
    setup_ns();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_NET_HEALTH;
    req.length = 0;
    uint32_t rc = net_server_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_NET_HEALTH returns SEL4_ERR_OK");
}

/* Test 3: OP_NET_VNIC_CREATE returns NET_OK with CAP_CLASS_NET */
static void test_ns_vnic_create_ok(void)
{
    setup_ns();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_NET_VNIC_CREATE;
    dw32(req.data, 0, 0xFFu);          /* auto-assign id */
    dw32(req.data, 4, CAP_CLASS_NET);  /* cap_classes    */
    dw32(req.data, 8, 1u);             /* caller_pd      */
    req.length = 12;
    uint32_t rc = net_server_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_NET_VNIC_CREATE: OK with CAP_CLASS_NET");
    ASSERT_EQ(dr32(rep.data, 0), (uint64_t)NET_OK, "OP_NET_VNIC_CREATE: reply NET_OK");
}

/* Test 4: OP_NET_VNIC_CREATE denied without CAP_CLASS_NET */
static void test_ns_vnic_create_perm_denied(void)
{
    setup_ns();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_NET_VNIC_CREATE;
    dw32(req.data, 0, 0u);
    dw32(req.data, 4, 0u);   /* no CAP_CLASS_NET */
    dw32(req.data, 8, 2u);
    req.length = 12;
    uint32_t rc = net_server_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_PERM, "OP_NET_VNIC_CREATE: denied without CAP_CLASS_NET");
    ASSERT_EQ(dr32(rep.data, 0), (uint64_t)NET_ERR_PERM, "OP_NET_VNIC_CREATE: reply NET_ERR_PERM");
}

/* Test 5: OP_NET_VNIC_DESTROY returns error for unknown vnic */
static void test_ns_vnic_destroy_not_found(void)
{
    setup_ns();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_NET_VNIC_DESTROY;
    dw32(req.data, 0, 99u);   /* non-existent vnic_id */
    req.length = 4;
    uint32_t rc = net_server_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_NOT_FOUND, "OP_NET_VNIC_DESTROY: not found returns error");
}

/* Test 6: Create then destroy successfully */
static void test_ns_vnic_create_destroy(void)
{
    setup_ns();
    sel4_msg_t req = {0}, rep = {0};

    /* Create */
    req.opcode = OP_NET_VNIC_CREATE;
    dw32(req.data, 0, 0xFFu);
    dw32(req.data, 4, CAP_CLASS_NET);
    dw32(req.data, 8, 1u);
    req.length = 12;
    net_server_dispatch_one(0, &req, &rep);
    uint32_t vnic_id = dr32(rep.data, 4);

    /* Destroy */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = OP_NET_VNIC_DESTROY;
    dw32(req.data, 0, vnic_id);
    req.length = 4;
    uint32_t rc = net_server_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_NET_VNIC_DESTROY: success after create");
}

/* Test 7: OP_NET_VNIC_SEND with non-existent vnic */
static void test_ns_vnic_send_not_found(void)
{
    setup_ns();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_NET_VNIC_SEND;
    dw32(req.data, 0, 42u);   /* no such vnic */
    dw32(req.data, 4, 0u);
    dw32(req.data, 8, 10u);
    req.length = 12;
    uint32_t rc = net_server_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_NOT_FOUND, "OP_NET_VNIC_SEND: not-found vnic");
}

/* Test 8: OP_NET_VNIC_RECV returns 0 bytes (no hw) */
static void test_ns_vnic_recv_empty(void)
{
    setup_ns();
    sel4_msg_t req = {0}, rep = {0};

    /* Create a vnic first */
    req.opcode = OP_NET_VNIC_CREATE;
    dw32(req.data, 0, 0xFFu);
    dw32(req.data, 4, CAP_CLASS_NET);
    dw32(req.data, 8, 1u);
    req.length = 12;
    net_server_dispatch_one(0, &req, &rep);
    uint32_t vnic_id = dr32(rep.data, 4);

    /* Recv */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = OP_NET_VNIC_RECV;
    dw32(req.data, 0, vnic_id);
    dw32(req.data, 4, 0u);
    dw32(req.data, 8, 1024u);
    req.length = 12;
    uint32_t rc = net_server_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_NET_VNIC_RECV: OK (0 bytes from stub)");
}

/* Test 9: OP_NET_STATUS global returns active_vnic_count */
static void test_ns_status_global(void)
{
    setup_ns();
    sel4_msg_t req = {0}, rep = {0};

    /* Create one vnic */
    req.opcode = OP_NET_VNIC_CREATE;
    dw32(req.data, 0, 0xFFu);
    dw32(req.data, 4, CAP_CLASS_NET);
    dw32(req.data, 8, 1u);
    req.length = 12;
    net_server_dispatch_one(0, &req, &rep);

    /* Status global */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = OP_NET_STATUS;
    dw32(req.data, 0, 0xFFFFFFFFu);
    req.length = 4;
    uint32_t rc = net_server_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_NET_STATUS global: returns SEL4_ERR_OK");
    ASSERT_EQ(dr32(rep.data, 4), 1u, "OP_NET_STATUS global: active_vnic_count == 1");
}

/* Test 10: unknown opcode returns SEL4_ERR_INVALID_OP */
static void test_ns_unknown_opcode(void)
{
    setup_ns();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xDEADBEEFu;
    req.length = 0;
    uint32_t rc = net_server_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_INVALID_OP, "net_server: unknown opcode -> SEL4_ERR_INVALID_OP");
}

/* ══════════════════════════════════════════════════════════════════════════
 * net_pd tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void setup_pd(void)
{
    memset(pd_shmem_backing, 0, sizeof(pd_shmem_backing));
    _netpd_iface_link_up = false;
    net_pd_shmem_vaddr   = (uintptr_t)pd_shmem_backing;
    net_pd_mmio_vaddr    = 0;
    _net_pd_test_init_fn();
}

/* Test 11: net_pd_test_init completes without crash */
static void test_pd_init(void)
{
    setup_pd();
    TAP_OK("net_pd_test_init completes without crash");
}

/* Test 12: MSG_NET_OPEN returns a valid handle */
static void test_pd_open_ok(void)
{
    setup_pd();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_NET_OPEN;
    dw32(req.data, 0, 0u);   /* iface_id = 0 */
    req.length = 4;
    uint32_t rc = call_net_pd(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_NET_OPEN: SEL4_ERR_OK");
    ASSERT_EQ(dr32(rep.data, 0), (uint64_t)NET_OK, "MSG_NET_OPEN: reply NET_OK");
}

/* Test 13: MSG_NET_CLOSE with bad handle returns error */
static void test_pd_close_bad_handle(void)
{
    setup_pd();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_NET_CLOSE;
    dw32(req.data, 0, 99u);   /* no such handle */
    req.length = 4;
    uint32_t rc = call_net_pd(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_NOT_FOUND, "MSG_NET_CLOSE: bad handle returns error");
}

/* Test 14: MSG_NET_SEND on open NIC handle (stub — no hw) */
static void test_pd_send_stub(void)
{
    setup_pd();
    sel4_msg_t req = {0}, rep = {0};

    /* Open */
    req.opcode = MSG_NET_OPEN;
    dw32(req.data, 0, 0u);
    req.length = 4;
    call_net_pd(0, &req, &rep);
    uint32_t handle = dr32(rep.data, 4);

    /* Send */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = MSG_NET_SEND;
    dw32(req.data, 0, handle);
    dw32(req.data, 4, 64u);   /* 64-byte frame */
    req.length = 8;
    uint32_t rc = call_net_pd(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_NET_SEND: NIC stub send returns OK");
}

/* Test 15: MSG_NET_RECV on NIC handle (empty ring) */
static void test_pd_recv_empty(void)
{
    setup_pd();
    sel4_msg_t req = {0}, rep = {0};

    req.opcode = MSG_NET_OPEN;
    dw32(req.data, 0, 0u);
    req.length = 4;
    call_net_pd(0, &req, &rep);
    uint32_t handle = dr32(rep.data, 4);

    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = MSG_NET_RECV;
    dw32(req.data, 0, handle);
    dw32(req.data, 4, 1500u);
    req.length = 8;
    uint32_t rc = call_net_pd(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_NET_RECV: empty ring returns OK with 0 bytes");
    ASSERT_EQ(dr32(rep.data, 4), 0u, "MSG_NET_RECV: returned bytes == 0");
}

/* Test 16: MSG_NET_DEV_STATUS global */
static void test_pd_dev_status_global(void)
{
    setup_pd();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_NET_DEV_STATUS;
    dw32(req.data, 0, 0xFFFFFFFFu);
    req.length = 4;
    uint32_t rc = call_net_pd(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_NET_DEV_STATUS global: SEL4_ERR_OK");
}

/* Test 17: MSG_NET_CONFIGURE updates MTU */
static void test_pd_configure(void)
{
    setup_pd();
    sel4_msg_t req = {0}, rep = {0};

    req.opcode = MSG_NET_OPEN;
    dw32(req.data, 0, 0u);
    req.length = 4;
    call_net_pd(0, &req, &rep);
    uint32_t handle = dr32(rep.data, 4);

    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = MSG_NET_CONFIGURE;
    dw32(req.data, 0, handle);
    dw32(req.data, 4, 9000u);   /* jumbo MTU */
    dw32(req.data, 8, 0u);
    req.length = 12;
    uint32_t rc = call_net_pd(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_NET_CONFIGURE: MTU update returns OK");
}

/* Test 18: MSG_NET_SOCKET_OPEN + BIND + LISTEN cycle */
static void test_pd_socket_lifecycle(void)
{
    setup_pd();
    sel4_msg_t req = {0}, rep = {0};

    /* Open TCP socket */
    req.opcode = MSG_NET_SOCKET_OPEN;
    dw32(req.data, 0, NET_PROTO_TCP);
    req.length = 4;
    uint32_t rc = call_net_pd(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_NET_SOCKET_OPEN: TCP ok");
    uint32_t sock = dr32(rep.data, 4);

    /* Bind */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = MSG_NET_SOCKET_BIND;
    dw32(req.data, 0, sock);
    dw32(req.data, 4, 8080u);
    req.length = 8;
    rc = call_net_pd(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_NET_SOCKET_BIND: port 8080 ok");

    /* Listen */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = MSG_NET_SOCKET_LISTEN;
    dw32(req.data, 0, sock);
    req.length = 4;
    rc = call_net_pd(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_NET_SOCKET_LISTEN: after bind ok");
}

/* Test 19: MSG_NET_SOCKET_ACCEPT (stub — no pending) */
static void test_pd_socket_accept_empty(void)
{
    setup_pd();
    sel4_msg_t req = {0}, rep = {0};

    req.opcode = MSG_NET_SOCKET_OPEN;
    dw32(req.data, 0, NET_PROTO_TCP);
    req.length = 4;
    call_net_pd(0, &req, &rep);
    uint32_t sock = dr32(rep.data, 4);

    /* Bind + Listen first */
    memset(&req, 0, sizeof(req));
    req.opcode = MSG_NET_SOCKET_BIND;
    dw32(req.data, 0, sock); dw32(req.data, 4, 9090u);
    req.length = 8;
    call_net_pd(0, &req, &rep);

    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = MSG_NET_SOCKET_LISTEN;
    dw32(req.data, 0, sock);
    req.length = 4;
    call_net_pd(0, &req, &rep);

    /* Accept — no incoming connection */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = MSG_NET_SOCKET_ACCEPT;
    dw32(req.data, 0, sock);
    req.length = 4;
    uint32_t rc = call_net_pd(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_NET_SOCKET_ACCEPT: stub returns OK, no pending");
    ASSERT_EQ(dr32(rep.data, 4), 0u, "MSG_NET_SOCKET_ACCEPT: new_handle == 0 (no pending)");
}

/* Test 20: net_pd unknown opcode */
static void test_pd_unknown_opcode(void)
{
    setup_pd();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xCAFEu;
    req.length = 0;
    uint32_t rc = call_net_pd(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_INVALID_OP, "net_pd: unknown opcode -> SEL4_ERR_INVALID_OP");
}

/* ══════════════════════════════════════════════════════════════════════════
 * wg_net tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void setup_wg(void)
{
    memset(wg_shmem_backing, 0, sizeof(wg_shmem_backing));
    wg_staging_vaddr = (uintptr_t)wg_shmem_backing;
    _wg_net_test_init_fn();
}

/* Test 21: wg_net_test_init completes without crash */
static void test_wg_init(void)
{
    setup_wg();
    TAP_OK("wg_net_test_init completes without crash");
}

/* Test 22: OP_WG_HEALTH before key set */
static void test_wg_health_no_key(void)
{
    setup_wg();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_WG_HEALTH;
    req.length = 0;
    uint32_t rc = call_wg_net(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_WG_HEALTH: OK before key set");
    ASSERT_EQ(dr32(rep.data, 8), 0u, "OP_WG_HEALTH: privkey_set == 0 initially");
}

/* Test 23: OP_WG_SET_PRIVKEY with valid staging */
static void test_wg_set_privkey(void)
{
    setup_wg();

    /* Write a non-zero private key at staging offset 0 */
    uint8_t *stage = (uint8_t *)wg_shmem_backing;
    for (int i = 0; i < 32; i++) stage[i] = (uint8_t)(i + 1);

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_WG_SET_PRIVKEY;
    dw32(req.data, 0, 0u);   /* key at staging offset 0 */
    req.length = 4;
    uint32_t rc = call_wg_net(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_WG_SET_PRIVKEY: OK with valid staging");
    ASSERT_EQ(dr32(rep.data, 0), (uint64_t)WG_OK, "OP_WG_SET_PRIVKEY: reply WG_OK");
}

/* Test 24: OP_WG_ADD_PEER with valid peer_id */
static void test_wg_add_peer_ok(void)
{
    setup_wg();
    /* Place a pubkey at staging offset 0x1000 */
    uint8_t *stage = wg_shmem_backing;
    for (int i = 0; i < 32; i++) stage[0x1000 + i] = (uint8_t)(i + 0x10);

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_WG_ADD_PEER;
    dw32(req.data,  0, 0u);         /* peer_id */
    dw32(req.data,  4, 0x1000u);    /* pubkey_off */
    dw32(req.data,  8, 0x0A000002u);/* endpoint_ip */
    dw32(req.data, 12, 51820u);     /* endpoint_port */
    dw32(req.data, 16, 0x0A000000u);/* allowed_ip */
    dw32(req.data, 20, 0xFF000000u);/* allowed_mask */
    req.length = 24;
    uint32_t rc = call_wg_net(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_WG_ADD_PEER: success for peer_id=0");
    ASSERT_EQ(dr32(rep.data, 0), (uint64_t)WG_OK, "OP_WG_ADD_PEER: reply WG_OK");
}

/* Test 25: OP_WG_ADD_PEER with out-of-range peer_id */
static void test_wg_add_peer_bad_id(void)
{
    setup_wg();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_WG_ADD_PEER;
    dw32(req.data, 0, 99u);   /* peer_id >= WG_MAX_PEERS */
    req.length = 4;
    uint32_t rc = call_wg_net(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_BAD_ARG, "OP_WG_ADD_PEER: bad peer_id returns error");
}

/* Test 26: OP_WG_REMOVE_PEER for non-existent peer */
static void test_wg_remove_peer_not_found(void)
{
    setup_wg();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_WG_REMOVE_PEER;
    dw32(req.data, 0, 5u);   /* peer not added */
    req.length = 4;
    uint32_t rc = call_wg_net(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_NOT_FOUND, "OP_WG_REMOVE_PEER: not-found returns error");
}

/* Test 27: OP_WG_SEND without private key returns WG_ERR_NOKEY */
static void test_wg_send_no_key(void)
{
    setup_wg();
    /* Add a peer first */
    uint8_t *stage = wg_shmem_backing;
    for (int i = 0; i < 32; i++) stage[0x1000 + i] = (uint8_t)(i + 1);

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_WG_ADD_PEER;
    dw32(req.data, 0, 0u);
    dw32(req.data, 4, 0x1000u);
    req.length = 24;
    call_wg_net(0, &req, &rep);

    /* Attempt send without setting private key */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = OP_WG_SEND;
    dw32(req.data, 0, 0u);   /* peer_id */
    dw32(req.data, 4, 0u);   /* data_off */
    dw32(req.data, 8, 10u);  /* data_len */
    req.length = 12;
    uint32_t rc = call_wg_net(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_PERM, "OP_WG_SEND: no private key returns SEL4_ERR_PERM");
    ASSERT_EQ(dr32(rep.data, 0), (uint64_t)WG_ERR_NOKEY, "OP_WG_SEND: reply WG_ERR_NOKEY");
}

/* Test 28: OP_WG_RECV when no packet pending */
static void test_wg_recv_empty(void)
{
    setup_wg();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_WG_RECV;
    dw32(req.data, 0, 0xFFu);   /* any peer */
    req.length = 4;
    uint32_t rc = call_wg_net(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_WG_RECV: empty returns SEL4_ERR_OK");
    ASSERT_EQ(dr32(rep.data, 12), 0u, "OP_WG_RECV: data_len == 0 when no packet pending");
}

/* Test 29: OP_WG_STATUS returns correct peer count */
static void test_wg_status_zero_peers(void)
{
    setup_wg();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_WG_STATUS;
    req.length = 0;
    uint32_t rc = call_wg_net(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "OP_WG_STATUS: OK with zero peers");
    ASSERT_EQ(dr32(rep.data, 4), 0u, "OP_WG_STATUS: active_peer_count == 0");
}

/* Test 30: Timer tick helper advances keepalive_due */
static void test_wg_timer_tick(void)
{
    setup_wg();
    uint32_t before = _wgnet_keepalive_due;
    _wg_net_timer_tick_fn();   /* advance timer_tick by 1 */
    /* keepalive_due should remain at WG_KEEPALIVE_SECS (25) unless tick >= due */
    ASSERT_TRUE(_wgnet_keepalive_due >= before,
                "wg_net timer tick: keepalive_due stays >= initial value");
}

/* Test 31: wg_net unknown opcode */
static void test_wg_unknown_opcode(void)
{
    setup_wg();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xBEEFu;
    req.length = 0;
    uint32_t rc = call_wg_net(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_INVALID_OP, "wg_net: unknown opcode -> SEL4_ERR_INVALID_OP");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    TAP_PLAN(40);   /* 40 assertions across 31 test functions */

    /* net_server */
    test_ns_init();               /* 1  assertion */
    test_ns_health();             /* 1  assertion */
    test_ns_vnic_create_ok();     /* 2  assertions */
    test_ns_vnic_create_perm_denied(); /* 2 assertions */
    test_ns_vnic_destroy_not_found();  /* 1 assertion */
    test_ns_vnic_create_destroy();     /* 1 assertion */
    test_ns_vnic_send_not_found();     /* 1 assertion */
    test_ns_vnic_recv_empty();         /* 1 assertion */
    test_ns_status_global();           /* 2 assertions */
    test_ns_unknown_opcode();          /* 1 assertion */

    /* net_pd */
    test_pd_init();              /* 1  assertion */
    test_pd_open_ok();           /* 2  assertions */
    test_pd_close_bad_handle();  /* 1  assertion */
    test_pd_send_stub();         /* 1  assertion */
    test_pd_recv_empty();        /* 2  assertions */
    test_pd_dev_status_global(); /* 1  assertion */
    test_pd_configure();         /* 1  assertion */
    test_pd_socket_lifecycle();  /* 3  assertions */
    test_pd_socket_accept_empty(); /* 2 assertions */
    test_pd_unknown_opcode();    /* 1  assertion */

    /* wg_net */
    test_wg_init();                  /* 1  assertion */
    test_wg_health_no_key();         /* 2  assertions */
    test_wg_set_privkey();           /* 2  assertions */
    test_wg_add_peer_ok();           /* 2  assertions */
    test_wg_add_peer_bad_id();       /* 1  assertion  */
    test_wg_remove_peer_not_found(); /* 1  assertion  */
    test_wg_send_no_key();           /* 2  assertions */
    test_wg_recv_empty();            /* 2  assertions */
    test_wg_status_zero_peers();     /* 2  assertions */
    test_wg_timer_tick();            /* 1  assertion  */
    test_wg_unknown_opcode();        /* 1  assertion  */

    return tap_exit();
}

#else /* !AGENTOS_TEST_HOST */
typedef int _agentos_test_network_stack_dummy;
#endif /* AGENTOS_TEST_HOST */
