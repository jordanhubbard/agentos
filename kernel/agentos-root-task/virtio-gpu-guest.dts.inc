/* Guest IPA faults into libvmm. No physical GPU frame is mapped here.
 * SPI 22 is virtual INTID 54; root grants no hardware GPU IRQ to a VMM. */
virtio_mmio@a040000 {
    compatible = "virtio,mmio";
    dma-coherent;
    reg = <0x00 0xa040000 0x00 0x1000>;
    interrupt-parent = <0x8001>;
    interrupts = <0x00 0x16 0x01>;
};
