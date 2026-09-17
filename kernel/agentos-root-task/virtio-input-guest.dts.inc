/* Faulting guest IPAs backed by the input virtualizer, never host MMIO.
 * Virtual SPI 23/24 are INTID 55/56. */
virtio_mmio@a050000 {
    compatible = "virtio,mmio";
    dma-coherent;
    reg = <0x00 0xa050000 0x00 0x1000>;
    interrupt-parent = <0x8001>;
    interrupts = <0x00 0x17 0x01>;
};
virtio_mmio@a060000 {
    compatible = "virtio,mmio";
    dma-coherent;
    reg = <0x00 0xa060000 0x00 0x1000>;
    interrupt-parent = <0x8001>;
    interrupts = <0x00 0x18 0x01>;
};
