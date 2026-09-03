/*
 * agentOS serial_virt queue ABI and guest virtio-console placement.
 *
 * The guest-visible device faults into linux_vmm. Its RX/TX byte queues use
 * the sDDF serial queue ABI; no guest maps the physical PL011.
 */

#ifndef AOS_PLATFORM_SERIAL_LAYOUT_H
#define AOS_PLATFORM_SERIAL_LAYOUT_H

#define AOS_SERIAL_RX_CAPACITY             4096u
#define AOS_SERIAL_TX_CAPACITY            65536u

/* GIC SPI 21 -> INTID 53. SPI 16-20 are already assigned. */
#define AOS_VIRTIO_CONSOLE_GUEST_IPA  0x0A030000UL
#define AOS_VIRTIO_CONSOLE_MMIO_SIZE      0x1000UL
#define AOS_VIRTIO_CONSOLE_VIRQ                 53u
#define AOS_VIRTIO_CONSOLE_DTB_SPI              21u

#endif /* AOS_PLATFORM_SERIAL_LAYOUT_H */
