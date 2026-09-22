/* Real network configuration callback through the real MMIO dispatcher. */
#define main mmio_regression_main
#include "test_virtio_mmio.c"
#undef main
void vmm_notify(seL4_CPtr cap);
#include "../../libvmm/src/virtio/net.c"
int main(void)
{
    const uint8_t mac[6]={2,0x11,0x22,0x33,0x44,0x55};
    struct virtio_net_device net={0};
    memcpy(net.config.mac,mac,sizeof(mac));
    virtio_device_funs_t config_functions={.get_device_config=virtio_net_get_device_config};
    net.virtio_device.device_data=&net;
    net.virtio_device.funs=&config_functions;
    for (unsigned i=0;i<6;++i) {
        seL4_UserContext regs={0};
        assert(virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_CONFIG+i,2,&regs,&net.virtio_device));
        assert(regs.x0==mac[i]);
    }
    seL4_UserContext regs={0};
    assert(virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_CONFIG,0,&regs,&net.virtio_device));
    assert(regs.x0==UINT32_C(0x33221102));
    assert(virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_CONFIG+4,0,&regs,&net.virtio_device));
    assert(regs.x0==UINT32_C(0x5544));
    puts("PASS: real virtio-net MAC config retains each byte through MMIO");
    return 0;
}
