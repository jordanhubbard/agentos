/*
 * Guest virtio-net path: DTB + VMM fault wiring + simulated TX/RX.
 *
 * Host-only. Does not prove a Linux boot. It does assert:
 *   1. Guest DTB nodes advertise IPA 0xa010000 / SPI 18
 *   2. linux_vmm pumps after fault_handle
 *   3. QEMU virtio-mmio page does not cover the emulated IPA
 *   4. A guest-shaped virtq TX walks virtio → aos_net_virt_pump → guest RX
 *
 * gcc -I platform/include -I tests/platform -DAOS_REPO_ROOT='"/path/"' \
 *     tests/platform/test_virtio_net_guest_path.c \
 *     tests/platform/virtio_mmio_net_emu.c \
 *     platform/net-virt/net_virt_pump.c \
 *     -o test_virtio_net_guest_path
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <platform/net_layout.h>
#include <platform/net_virt_pump.h>
#include "virtio_mmio_net_emu.h"

#ifndef AOS_REPO_ROOT
#define AOS_REPO_ROOT "./"
#endif

static int g_failed;
static int g_testno;

static int tap_ok(int cond, const char *name)
{
    g_testno++;
    if (cond) {
        printf("ok %d - %s\n", g_testno, name);
        return 0;
    }
    printf("not ok %d - %s\n", g_testno, name);
    g_failed++;
    return 1;
}

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("# FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failed++; \
        return 1; \
    } \
} while (0)

static char *read_file(const char *rel)
{
    char path[1024];
    FILE *f;
    long sz;
    char *buf;
    size_t n;

    snprintf(path, sizeof(path), "%s%s", AOS_REPO_ROOT, rel);
    f = fopen(path, "rb");
    if (!f) {
        printf("# cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static const char *find_node(const char *text, const char *node)
{
    const char *p = strstr(text, node);
    return p;
}

static int node_has(const char *node, const char *needle, size_t window)
{
    char tmp[512];
    size_t n;

    if (!node) {
        return 0;
    }
    n = window;
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1u;
    }
    memcpy(tmp, node, n);
    tmp[n] = '\0';
    return strstr(tmp, needle) != NULL;
}

static int dts_emulated_net_ok(const char *rel)
{
    char *text = read_file(rel);
    const char *node;
    int ok;

    if (!text) {
        return 0;
    }
    node = find_node(text, "virtio_mmio@a010000");
    ok = node
      && node_has(node, "compatible = \"virtio,mmio\"", 400)
      && node_has(node, "0xa010000", 400)
      && node_has(node, "0x1000", 400)
      && node_has(node, "0x12", 400);
    free(text);
    return ok;
}

static int src_contains_in_order(const char *rel, const char *first, const char *second)
{
    char *text = read_file(rel);
    const char *a;
    const char *b;
    int ok;

    if (!text) {
        return 0;
    }
    a = strstr(text, first);
    b = a ? strstr(a, second) : NULL;
    ok = a != NULL && b != NULL;
    free(text);
    return ok;
}

static int src_contains(const char *rel, const char *needle)
{
    char *text = read_file(rel);
    int ok;

    if (!text) {
        return 0;
    }
    ok = strstr(text, needle) != NULL;
    free(text);
    return ok;
}

static int test_abi(void)
{
    CHECK(AOS_VIRTIO_NET_GUEST_IPA == 0x0A010000UL);
    CHECK(AOS_VIRTIO_NET_MMIO_SIZE == 0x1000UL);
    CHECK(AOS_VIRTIO_NET_VIRQ == 50u);
    CHECK(AOS_VIRTIO_NET_DTB_SPI == 18u);
    CHECK(AOS_VIRTIO_NET_VIRQ == 32u + AOS_VIRTIO_NET_DTB_SPI);
    CHECK(AOS_VIRTIO_NET_GUEST_IPA != 0x0A000000UL);
    CHECK(AOS_VIRTIO_NET_GUEST_IPA >= 0x0A000000UL + 0x1000UL);
    CHECK(AOS_VIRTIO_NET_GUEST_IPA >= 0x0A000000UL + 32u * 0x200u);
    CHECK(AOS_VIRTIO_NET_MAC0 == 0x02u);
    CHECK(AOS_VIRTIO_NET_HDR_LEN == 12u);
    return tap_ok(1, "abi IPA 0x0A010000 IRQ 50 (SPI 18) outside QEMU page");
}

static int test_dtb(const char *rel, const char *name)
{
    return tap_ok(dts_emulated_net_ok(rel), name);
}

static int test_vmm_fault_path(void)
{
    int init_ok = src_contains("kernel/agentos-root-task/src/linux_vmm.c",
                               "aos_vmm_virtio_net_init");
    int after = src_contains_in_order(
        "kernel/agentos-root-task/src/linux_vmm.c",
        "fault_handle(vcpu_id, msginfo)",
        "aos_vmm_virtio_net_after_fault()");
    int ipa = src_contains("kernel/agentos-root-task/src/linux_vmm.c",
                           "0x0A010000");
    return tap_ok(init_ok && after && ipa,
                  "linux_vmm: init + fault_handle then after_fault for IPA 0x0A010000");
}

static int test_qemu_page_unmapped(void)
{
    int page = src_contains("kernel/agentos-root-task/src/main.c",
                            "VIRTIO_MMIO_PAGE_PA  0x0A000000UL");
    int comment = src_contains("kernel/agentos-root-task/src/main.c",
                               "0x0A010000");
    int not_mapped = !src_contains("kernel/agentos-root-task/src/main.c",
                                   "0x0A010000UL");
    return tap_ok(page && comment && not_mapped,
                  "root task maps QEMU 0x0A000000 page only; emulated IPA stays unmapped");
}

#define VQ_NUM 8u

static uint8_t g_region[AOS_NET_CLIENT_STRIDE];

static aos_virtq_desc_t rx_desc[VQ_NUM];
static aos_virtq_desc_t tx_desc[VQ_NUM];
static struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_NUM];
} rx_avail, tx_avail;
static struct {
    uint16_t flags;
    uint16_t idx;
    aos_virtq_used_elem_t ring[VQ_NUM];
} rx_used, tx_used;

static uint8_t rx_pkt[2048];
static uint8_t tx_pkt[2048];

static int mmio_w(aos_virtio_mmio_net_t *d, uint32_t off, uint32_t val)
{
    return aos_virtio_mmio_net_write(d, off, val);
}

static int mmio_r(aos_virtio_mmio_net_t *d, uint32_t off, uint32_t *out)
{
    return aos_virtio_mmio_net_read(d, off, out);
}

static int write_ptr(aos_virtio_mmio_net_t *d, uint32_t lo, uint32_t hi, const void *p)
{
    uintptr_t v = (uintptr_t)p;
    if (mmio_w(d, lo, (uint32_t)v) != 0) {
        return -1;
    }
    return mmio_w(d, hi, (uint32_t)(v >> 32));
}

static int setup_queue(aos_virtio_mmio_net_t *d, uint32_t q,
                       aos_virtq_desc_t *desc, void *avail, void *used)
{
    if (mmio_w(d, AOS_VIRTIO_REG_QUEUE_SEL, q) != 0) {
        return -1;
    }
    if (mmio_w(d, AOS_VIRTIO_REG_QUEUE_NUM, VQ_NUM) != 0) {
        return -1;
    }
    if (write_ptr(d, AOS_VIRTIO_REG_QUEUE_DESC_LOW, AOS_VIRTIO_REG_QUEUE_DESC_HIGH, desc) != 0) {
        return -1;
    }
    if (write_ptr(d, AOS_VIRTIO_REG_QUEUE_AVAIL_LOW, AOS_VIRTIO_REG_QUEUE_AVAIL_HIGH, avail) != 0) {
        return -1;
    }
    if (write_ptr(d, AOS_VIRTIO_REG_QUEUE_USED_LOW, AOS_VIRTIO_REG_QUEUE_USED_HIGH, used) != 0) {
        return -1;
    }
    return mmio_w(d, AOS_VIRTIO_REG_QUEUE_READY, 1u);
}

static int guest_probe(aos_virtio_mmio_net_t *d)
{
    uint32_t magic = 0, ver = 0, id = 0, feat = 0, st = 0, mac0 = 0, mac1 = 0;
    uint8_t mac[6];

    CHECK(mmio_r(d, AOS_VIRTIO_REG_MAGIC, &magic) == 0);
    CHECK(magic == AOS_VIRTIO_MMIO_MAGIC);
    CHECK(mmio_r(d, AOS_VIRTIO_REG_VERSION, &ver) == 0);
    CHECK(ver == AOS_VIRTIO_MMIO_VERSION);
    CHECK(mmio_r(d, AOS_VIRTIO_REG_DEVICE_ID, &id) == 0);
    CHECK(id == AOS_VIRTIO_NET_DEVICE_ID);

    CHECK(mmio_w(d, AOS_VIRTIO_REG_STATUS, AOS_VIRTIO_S_ACKNOWLEDGE) == 0);
    CHECK(d->probed == 1);

    CHECK(mmio_w(d, AOS_VIRTIO_REG_STATUS,
                 AOS_VIRTIO_S_ACKNOWLEDGE | AOS_VIRTIO_S_DRIVER) == 0);

    CHECK(mmio_w(d, AOS_VIRTIO_REG_DEVICE_FEATURES_SEL, 0) == 0);
    CHECK(mmio_r(d, AOS_VIRTIO_REG_DEVICE_FEATURES, &feat) == 0);
    CHECK((feat & (1u << AOS_VIRTIO_NET_F_MAC)) != 0);
    CHECK((feat & (1u << AOS_VIRTIO_NET_F_MRG_RXBUF)) != 0);
    CHECK(mmio_w(d, AOS_VIRTIO_REG_DRIVER_FEATURES_SEL, 0) == 0);
    CHECK(mmio_w(d, AOS_VIRTIO_REG_DRIVER_FEATURES, feat) == 0);

    CHECK(mmio_w(d, AOS_VIRTIO_REG_DEVICE_FEATURES_SEL, 1) == 0);
    CHECK(mmio_r(d, AOS_VIRTIO_REG_DEVICE_FEATURES, &feat) == 0);
    CHECK(feat == 1u); /* VERSION_1 */
    CHECK(mmio_w(d, AOS_VIRTIO_REG_DRIVER_FEATURES_SEL, 1) == 0);
    CHECK(mmio_w(d, AOS_VIRTIO_REG_DRIVER_FEATURES, feat) == 0);

    CHECK(mmio_w(d, AOS_VIRTIO_REG_STATUS,
                 AOS_VIRTIO_S_ACKNOWLEDGE | AOS_VIRTIO_S_DRIVER |
                 AOS_VIRTIO_S_FEATURES_OK) == 0);
    CHECK(mmio_r(d, AOS_VIRTIO_REG_STATUS, &st) == 0);
    CHECK((st & AOS_VIRTIO_S_FEATURES_OK) != 0);

    CHECK(mmio_r(d, AOS_VIRTIO_REG_CONFIG, &mac0) == 0);
    CHECK(mmio_r(d, AOS_VIRTIO_REG_CONFIG + 4u, &mac1) == 0);
    mac[0] = (uint8_t)mac0;
    mac[1] = (uint8_t)(mac0 >> 8);
    mac[2] = (uint8_t)(mac0 >> 16);
    mac[3] = (uint8_t)(mac0 >> 24);
    mac[4] = (uint8_t)mac1;
    mac[5] = (uint8_t)(mac1 >> 8);
    CHECK(mac[0] == AOS_VIRTIO_NET_MAC0);
    CHECK(mac[5] == AOS_VIRTIO_NET_MAC5);

    memset(&rx_desc, 0, sizeof(rx_desc));
    memset(&tx_desc, 0, sizeof(tx_desc));
    memset(&rx_avail, 0, sizeof(rx_avail));
    memset(&tx_avail, 0, sizeof(tx_avail));
    memset(&rx_used, 0, sizeof(rx_used));
    memset(&tx_used, 0, sizeof(tx_used));
    memset(rx_pkt, 0, sizeof(rx_pkt));
    memset(tx_pkt, 0, sizeof(tx_pkt));

    /* RX first, TX last — then leave QueueSel on TX? Linux often finishes
     * on the last queue. We finish on RX so QueueSel != TX, proving notify
     * uses the written index. */
    CHECK(setup_queue(d, AOS_VIRTIO_NET_TX_VQ, tx_desc, &tx_avail, &tx_used) == 0);
    CHECK(setup_queue(d, AOS_VIRTIO_NET_RX_VQ, rx_desc, &rx_avail, &rx_used) == 0);
    CHECK(d->queue_sel == AOS_VIRTIO_NET_RX_VQ);

    CHECK(mmio_w(d, AOS_VIRTIO_REG_STATUS,
                 AOS_VIRTIO_S_ACKNOWLEDGE | AOS_VIRTIO_S_DRIVER |
                 AOS_VIRTIO_S_FEATURES_OK | AOS_VIRTIO_S_DRIVER_OK) == 0);
    CHECK(d->driver_ok == 1);
    return 0;
}

static void guest_offer_rx(void)
{
    rx_desc[0].addr = (uint64_t)(uintptr_t)rx_pkt;
    rx_desc[0].len = (uint32_t)sizeof(rx_pkt);
    rx_desc[0].flags = AOS_VIRTQ_DESC_F_WRITE;
    rx_desc[0].next = 0;
    rx_avail.ring[0] = 0;
    rx_avail.idx = 1;
}

static void guest_kick_tx_frame(const uint8_t *frame, uint16_t frame_len)
{
    memset(tx_pkt, 0, sizeof(tx_pkt));
    tx_pkt[10] = 1; /* num_buffers in mrg_rxbuf header */
    memcpy(tx_pkt + AOS_VIRTIO_NET_HDR_LEN, frame, frame_len);

    tx_desc[0].addr = (uint64_t)(uintptr_t)tx_pkt;
    tx_desc[0].len = (uint32_t)(AOS_VIRTIO_NET_HDR_LEN + frame_len);
    tx_desc[0].flags = 0;
    tx_desc[0].next = 0;
    tx_avail.ring[tx_avail.idx % VQ_NUM] = 0;
    tx_avail.idx++;
}

static int test_mmio_probe(void)
{
    aos_net_virt_t virt;
    aos_net_virt_client_t client;
    aos_virtio_mmio_net_t dev;
    uint8_t mac[6] = {
        AOS_VIRTIO_NET_MAC0, AOS_VIRTIO_NET_MAC1, AOS_VIRTIO_NET_MAC2,
        AOS_VIRTIO_NET_MAC3, AOS_VIRTIO_NET_MAC4, AOS_VIRTIO_NET_MAC5
    };

    memset(g_region, 0, sizeof(g_region));
    aos_net_virt_reset(&virt);
    aos_net_client_bind(g_region, 0u, &client);
    aos_net_client_init_buffers(&client);
    CHECK(aos_net_virt_add_client(&virt, &client) == 0);
    aos_virtio_mmio_net_init(&dev, &client, mac);
    CHECK(guest_probe(&dev) == 0);
    return tap_ok(1, "virtio-mmio probe: magic/version/net/MAC/FEATURES_OK/DRIVER_OK");
}

static int test_guest_tx_rx_loopback(void)
{
    aos_net_virt_t virt;
    aos_net_virt_client_t client;
    aos_virtio_mmio_net_t dev;
    uint8_t mac[6] = {
        AOS_VIRTIO_NET_MAC0, AOS_VIRTIO_NET_MAC1, AOS_VIRTIO_NET_MAC2,
        AOS_VIRTIO_NET_MAC3, AOS_VIRTIO_NET_MAC4, AOS_VIRTIO_NET_MAC5
    };
    uint8_t frame[64];
    uint32_t i;
    uint32_t delivered;
    uint16_t used_len;

    memset(g_region, 0, sizeof(g_region));
    memset(frame, 0, sizeof(frame));
    for (i = 0; i < 6u; i++) {
        frame[i] = 0xff;
    }
    frame[6] = AOS_VIRTIO_NET_MAC0;
    frame[7] = AOS_VIRTIO_NET_MAC1;
    frame[8] = AOS_VIRTIO_NET_MAC2;
    frame[9] = AOS_VIRTIO_NET_MAC3;
    frame[10] = AOS_VIRTIO_NET_MAC4;
    frame[11] = AOS_VIRTIO_NET_MAC5;
    frame[12] = 0x08;
    frame[13] = 0x00;
    for (i = 14; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(0xa0u + i);
    }

    aos_net_virt_reset(&virt);
    aos_net_client_bind(g_region, 0u, &client);
    aos_net_client_init_buffers(&client);
    client.tx_active->consumer_signalled = 1u;
    CHECK(aos_net_virt_add_client(&virt, &client) == 0);
    aos_virtio_mmio_net_init(&dev, &client, mac);
    CHECK(guest_probe(&dev) == 0);

    guest_offer_rx();
    guest_kick_tx_frame(frame, (uint16_t)sizeof(frame));

    /* QueueSel is still RX. Linux writes QUEUE_NOTIFY = TX index. */
    CHECK(dev.queue_sel == AOS_VIRTIO_NET_RX_VQ);
    CHECK(mmio_w(&dev, AOS_VIRTIO_REG_QUEUE_NOTIFY, AOS_VIRTIO_NET_TX_VQ) == 0);
    CHECK(tx_used.idx == 1);
    CHECK(tx_used.ring[0].len == (uint32_t)sizeof(frame));
    CHECK((uint16_t)(client.tx_active->tail - client.tx_active->head) == 1u);

    delivered = aos_virtio_mmio_net_after_fault(&dev, &virt);
    CHECK(delivered == 1u);
    CHECK(rx_used.idx == 1);
    used_len = (uint16_t)rx_used.ring[0].len;
    CHECK(used_len == (uint16_t)(AOS_VIRTIO_NET_HDR_LEN + sizeof(frame)));
    CHECK(rx_pkt[10] == 1); /* num_buffers */
    CHECK(memcmp(rx_pkt + AOS_VIRTIO_NET_HDR_LEN, frame, sizeof(frame)) == 0);
    CHECK((uint16_t)(client.rx_active->tail - client.rx_active->head) == 0u);
    CHECK((uint16_t)(client.tx_free->tail - client.tx_free->head) == (uint16_t)AOS_NET_CAPACITY);
    return tap_ok(1, "guest TX virtq → pump → guest RX (payload + virtio hdr)");
}

static int test_chained_tx_desc(void)
{
    aos_net_virt_t virt;
    aos_net_virt_client_t client;
    aos_virtio_mmio_net_t dev;
    uint8_t mac[6] = {
        AOS_VIRTIO_NET_MAC0, AOS_VIRTIO_NET_MAC1, AOS_VIRTIO_NET_MAC2,
        AOS_VIRTIO_NET_MAC3, AOS_VIRTIO_NET_MAC4, AOS_VIRTIO_NET_MAC5
    };
    uint8_t hdr[AOS_VIRTIO_NET_HDR_LEN];
    uint8_t frame[32];
    uint32_t delivered;

    memset(g_region, 0, sizeof(g_region));
    memset(hdr, 0, sizeof(hdr));
    hdr[10] = 1;
    memset(frame, 0x5c, sizeof(frame));

    aos_net_virt_reset(&virt);
    aos_net_client_bind(g_region, 0u, &client);
    aos_net_client_init_buffers(&client);
    client.tx_active->consumer_signalled = 1u;
    CHECK(aos_net_virt_add_client(&virt, &client) == 0);
    aos_virtio_mmio_net_init(&dev, &client, mac);
    CHECK(guest_probe(&dev) == 0);

    guest_offer_rx();
    tx_desc[0].addr = (uint64_t)(uintptr_t)hdr;
    tx_desc[0].len = AOS_VIRTIO_NET_HDR_LEN;
    tx_desc[0].flags = AOS_VIRTQ_DESC_F_NEXT;
    tx_desc[0].next = 1;
    tx_desc[1].addr = (uint64_t)(uintptr_t)frame;
    tx_desc[1].len = (uint32_t)sizeof(frame);
    tx_desc[1].flags = 0;
    tx_avail.ring[0] = 0;
    tx_avail.idx = 1;

    CHECK(mmio_w(&dev, AOS_VIRTIO_REG_QUEUE_NOTIFY, AOS_VIRTIO_NET_TX_VQ) == 0);
    delivered = aos_virtio_mmio_net_after_fault(&dev, &virt);
    CHECK(delivered == 1u);
    CHECK(memcmp(rx_pkt + AOS_VIRTIO_NET_HDR_LEN, frame, sizeof(frame)) == 0);
    return tap_ok(1, "chained TX desc (hdr + frame) loopback");
}

static int test_rx_kick_is_noop(void)
{
    aos_net_virt_t virt;
    aos_net_virt_client_t client;
    aos_virtio_mmio_net_t dev;
    uint8_t mac[6] = {
        AOS_VIRTIO_NET_MAC0, AOS_VIRTIO_NET_MAC1, AOS_VIRTIO_NET_MAC2,
        AOS_VIRTIO_NET_MAC3, AOS_VIRTIO_NET_MAC4, AOS_VIRTIO_NET_MAC5
    };

    memset(g_region, 0, sizeof(g_region));
    aos_net_virt_reset(&virt);
    aos_net_client_bind(g_region, 0u, &client);
    aos_net_client_init_buffers(&client);
    CHECK(aos_net_virt_add_client(&virt, &client) == 0);
    aos_virtio_mmio_net_init(&dev, &client, mac);
    CHECK(guest_probe(&dev) == 0);
    CHECK(mmio_w(&dev, AOS_VIRTIO_REG_QUEUE_NOTIFY, AOS_VIRTIO_NET_RX_VQ) == 0);
    CHECK(tx_used.idx == 0);
    CHECK((uint16_t)(client.tx_active->tail - client.tx_active->head) == 0u);
    return tap_ok(1, "RX QueueNotify is a no-op (RX is pulled after pump)");
}

int main(void)
{
    printf("TAP version 14\n");
    printf("# suite: virtio_net_guest_path (host pre-filter, not guest-boot proof)\n");

    (void)test_abi();
    (void)test_dtb("kernel/agentos-root-task/ubuntu-iso-overlay.dts.in",
                   "DTB ubuntu-iso-overlay.dts.in has virtio_mmio@a010000");
    (void)test_dtb("libvmm/examples/simple/board/qemu_virt_aarch64/overlay.dts",
                   "DTB buildroot overlay.dts has virtio_mmio@a010000");
    (void)test_dtb("libvmm/examples/simple/board/qemu_virt_aarch64/ubuntu-overlay.dts",
                   "DTB ubuntu-overlay.dts has virtio_mmio@a010000");
    (void)test_vmm_fault_path();
    (void)test_qemu_page_unmapped();
    (void)test_mmio_probe();
    (void)test_guest_tx_rx_loopback();
    (void)test_chained_tx_desc();
    (void)test_rx_kick_is_noop();

    printf("1..%d\n", g_testno);
    if (g_failed) {
        printf("# %d failed (host pre-filter only)\n", g_failed);
        return 1;
    }
    printf("# all virtio_net_guest_path tests passed (not boot-proven)\n");
    return 0;
}
