/*
 * blk_virt PD — NOT IN THE LIVE IMAGE.
 *
 * This pass linux_vmm keeps a private RAM disk + sDDF queues in BSS and
 * pumps them in-process (aos_blk_virt_pump). That is enough to call
 * virtio_mmio_blk_init without adding a PD to system_desc or IMAGES.
 *
 * QEMU virtio-blk (guest vda at 0x0A000200) stays the boot disk until
 * this backend is proven. Do not add this object to linux_vmm.elf or
 * the PD table.
 *
 * Next (after guest I/O is proven through the emulated device):
 *   1. blk_drv owns QEMU virtio-blk / the real disk.
 *   2. This file becomes the mux PD.
 *   3. The region is a shared MR into blk_virt + every VMM.
 *   4. Guest DTB drops the QEMU virtio-mmio@a000200 node.
 */
