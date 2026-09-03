/*
 * net_virt PD — NOT IN THE LIVE IMAGE.
 *
 * This pass linux_vmm maps a private 2 MB region at AOS_NET_SHMEM_VA and
 * pumps it in-process (aos_net_virt_pump). That is enough to call
 * virtio_mmio_net_init without adding a PD to system_desc or IMAGES.
 *
 * Next (after packets are proven through the emulated device):
 *   1. nic_drv owns QEMU virtio-net / the real NIC.
 *   2. This file becomes the mux PD.
 *   3. The 2 MB region is a shared MR into net_virt + every VMM.
 *
 * Do not add this object to linux_vmm.elf or the PD table.
 */
