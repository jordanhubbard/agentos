/*
 * Host-side virtio-mmio net + virtq TX/RX, matching libvmm's algorithms:
 *   libvmm/src/virtio/mmio.c  (register file)
 *   libvmm/src/virtio/net.c   (header strip/prepend, QueueNotify = written index)
 *
 * No seL4. Used by tests/platform to assert guest TX → sDDF pump → guest RX.
 * Not linked into the VMM; the live device remains libvmm virtio_mmio_net_init.
 */

#ifndef AOS_TEST_VIRTIO_MMIO_NET_EMU_H
#define AOS_TEST_VIRTIO_MMIO_NET_EMU_H

#include <stdint.h>
#include <platform/net_layout.h>
#include <platform/net_virt_pump.h>

#define AOS_VIRTIO_MMIO_MAGIC           0x74726976u  /* "virt" */
#define AOS_VIRTIO_MMIO_VERSION         2u
#define AOS_VIRTIO_NET_DEVICE_ID        1u
#define AOS_VIRTIO_MMIO_VENDOR_SEL4     0x344c6573u  /* "seL4" */

#define AOS_VIRTIO_REG_MAGIC            0x000u
#define AOS_VIRTIO_REG_VERSION          0x004u
#define AOS_VIRTIO_REG_DEVICE_ID        0x008u
#define AOS_VIRTIO_REG_VENDOR_ID        0x00cu
#define AOS_VIRTIO_REG_DEVICE_FEATURES  0x010u
#define AOS_VIRTIO_REG_DEVICE_FEATURES_SEL 0x014u
#define AOS_VIRTIO_REG_DRIVER_FEATURES  0x020u
#define AOS_VIRTIO_REG_DRIVER_FEATURES_SEL 0x024u
#define AOS_VIRTIO_REG_QUEUE_SEL        0x030u
#define AOS_VIRTIO_REG_QUEUE_NUM_MAX    0x034u
#define AOS_VIRTIO_REG_QUEUE_NUM        0x038u
#define AOS_VIRTIO_REG_QUEUE_READY      0x044u
#define AOS_VIRTIO_REG_QUEUE_NOTIFY     0x050u
#define AOS_VIRTIO_REG_INTERRUPT_STATUS 0x060u
#define AOS_VIRTIO_REG_INTERRUPT_ACK    0x064u
#define AOS_VIRTIO_REG_STATUS           0x070u
#define AOS_VIRTIO_REG_QUEUE_DESC_LOW   0x080u
#define AOS_VIRTIO_REG_QUEUE_DESC_HIGH  0x084u
#define AOS_VIRTIO_REG_QUEUE_AVAIL_LOW  0x090u
#define AOS_VIRTIO_REG_QUEUE_AVAIL_HIGH 0x094u
#define AOS_VIRTIO_REG_QUEUE_USED_LOW   0x0a0u
#define AOS_VIRTIO_REG_QUEUE_USED_HIGH  0x0a4u
#define AOS_VIRTIO_REG_CONFIG           0x100u

#define AOS_VIRTIO_S_ACKNOWLEDGE        1u
#define AOS_VIRTIO_S_DRIVER             2u
#define AOS_VIRTIO_S_DRIVER_OK          4u
#define AOS_VIRTIO_S_FEATURES_OK        8u

#define AOS_VIRTIO_NET_F_MAC            5u
#define AOS_VIRTIO_NET_F_MRG_RXBUF      15u
#define AOS_VIRTIO_F_VERSION_1          32u

#define AOS_VIRTIO_NET_RX_VQ            0u
#define AOS_VIRTIO_NET_TX_VQ            1u
#define AOS_VIRTIO_NET_NUM_VQ           2u
#define AOS_VIRTIO_QUEUE_SIZE_MAX       128u

/* virtio_net_hdr_mrg_rxbuf: libvmm always strips/prepends this. */
#define AOS_VIRTIO_NET_HDR_LEN          12u

#define AOS_VIRTQ_DESC_F_NEXT           1u
#define AOS_VIRTQ_DESC_F_WRITE          2u

typedef struct aos_virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} aos_virtq_desc_t;

typedef struct aos_virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} aos_virtq_avail_t;

typedef struct aos_virtq_used_elem {
    uint32_t id;
    uint32_t len;
} aos_virtq_used_elem_t;

typedef struct aos_virtq_used {
    uint16_t flags;
    uint16_t idx;
    aos_virtq_used_elem_t ring[];
} aos_virtq_used_t;

typedef struct aos_virtq {
    unsigned int num;
    aos_virtq_desc_t *desc;
    aos_virtq_avail_t *avail;
    aos_virtq_used_t *used;
} aos_virtq_t;

typedef struct aos_virtio_vq {
    aos_virtq_t virtq;
    int ready;
    uint16_t last_idx;
} aos_virtio_vq_t;

typedef struct aos_virtio_mmio_net {
    uint32_t status;
    uint32_t device_features_sel;
    uint32_t driver_features_sel;
    uint32_t queue_sel;
    uint32_t queue_notify;
    uint32_t interrupt_status;
    int features_happy;
    int probed;       /* guest wrote ACKNOWLEDGE */
    int driver_ok;
    uint8_t mac[6];
    aos_virtio_vq_t vqs[AOS_VIRTIO_NET_NUM_VQ];
    aos_net_virt_client_t client;
} aos_virtio_mmio_net_t;

void aos_virtio_mmio_net_init(aos_virtio_mmio_net_t *d,
                              const aos_net_virt_client_t *client,
                              const uint8_t mac[6]);

int aos_virtio_mmio_net_read(aos_virtio_mmio_net_t *d, uint32_t off, uint32_t *out);
int aos_virtio_mmio_net_write(aos_virtio_mmio_net_t *d, uint32_t off, uint32_t val);

/* Pull sDDF rx_active into the RX virtq (libvmm virtio_net_handle_rx). */
uint32_t aos_virtio_mmio_net_handle_rx(aos_virtio_mmio_net_t *d);

/*
 * VMM after-fault sequence: pump sDDF, then fill the guest RX virtq.
 * Returns packets delivered onto the RX virtq.
 */
uint32_t aos_virtio_mmio_net_after_fault(aos_virtio_mmio_net_t *d, aos_net_virt_t *virt);

#endif /* AOS_TEST_VIRTIO_MMIO_NET_EMU_H */
