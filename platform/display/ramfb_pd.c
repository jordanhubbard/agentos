#include <platform/display.h>
#include <platform/display_layout.h>
#include <platform/ramfb_mmio.h>
#include "system_desc.h"
#include "serial_log.h"
#include <sel4/sel4.h>

static aos_display_driver_t driver;
static aos_ramfb_mmio_t transport;
static aos_ramfb_io_t io;
static aos_display_meta_t metadata;
static void report(const char *text)
{
    static serial_log_t channel={.ep=PD_CNODE_SLOT_SERIAL_EP};
    serial_log_puts(&channel,text);
}
static int present(void *context,unsigned bank,uint32_t width,uint32_t height)
{
    (void)context;
    if (bank>1) return -1;
    int result=aos_ramfb_configure(&io,metadata.bank_physical[bank],
        metadata.bank_bytes,width,height,width*4u);
    static unsigned reported;
    if (!reported) {
        report(result==AOS_RAMFB_OK ? "[display] first frame configured\n" :
                                    "[display] FAIL: frame configuration\n");
        reported=1;
    }
    return result==AOS_RAMFB_OK ? 0 : -1;
}
void pd_main(seL4_CPtr endpoint,seL4_CPtr nameserver)
{
    (void)endpoint; (void)nameserver;
    metadata=*(const aos_display_meta_t *)AOS_DISPLAY_DMA_VA;
    if (metadata.magic!=AOS_DISPLAY_META_MAGIC || metadata.version!=1 ||
        metadata.bank_bytes!=AOS_DISPLAY_BANK_STRIDE ||
        aos_ramfb_mmio_init(&transport,(void *)AOS_DISPLAY_MMIO_VA,
            (void *)(AOS_DISPLAY_DMA_VA+AOS_DISPLAY_DMA_OFFSET),
            metadata.dma_physical+AOS_DISPLAY_DMA_OFFSET,100000,&io)!=0 ||
        aos_display_init(&driver,(void *)AOS_DISPLAY_QUEUE_VA,
            (void *)AOS_DISPLAY_BANK_VA,
            (void *)(AOS_DISPLAY_BANK_VA+AOS_DISPLAY_BANK_STRIDE),
            AOS_DISPLAY_BANK_STRIDE,present,0)!=0) {
        report("[display] FAIL: root-provisioned transport metadata\n");
        for (;;) seL4_Yield();
    }
    report("[display] private DMA and scanout banks ready\n");
    for (;;) {
        if (aos_display_pump(&driver))
            seL4_Signal(PD_CNODE_SLOT_DISPLAY_PEER_NOTIFY);
        else { seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_DISPLAY_WAIT,&badge); }
    }
}
