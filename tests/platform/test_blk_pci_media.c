#include <assert.h>
#include <stdio.h>
#include <platform/blk_host_layout.h>

int main(void)
{
    aos_blk_pci_set_t set = {.magic=AOS_BLK_PCI_INFO_MAGIC, .version=2, .count=2,
        .media={
            {.magic=AOS_BLK_PCI_INFO_MAGIC, .version=1,
             .offset={0,64,128}, .length={56,2,64}, .notify_multiplier=4},
            {.magic=AOS_BLK_PCI_INFO_MAGIC, .version=1,
             .offset={256,320,384}, .length={56,2,64}, .notify_multiplier=8},
        }};
    assert(aos_blk_pci_set_valid(&set));
    assert(aos_blk_pci_info_valid(&set.media[0])); /* retained v2 metadata ABI */
    assert(AOS_BLK_PCI_MEDIA_REGION_VA(0,0) == 0x06000000u);
    assert(AOS_BLK_PCI_MEDIA_REGION_VA(1,0) == 0x06004000u);
    assert(AOS_BLK_PCI_MEDIA_REGION_VA(0,2) + 4096u <=
           AOS_BLK_PCI_MEDIA_REGION_VA(1,0));
    assert(AGENTOS_BLK_MEDIA_DMA_OFF(1) + AGENTOS_BLK_MEDIA_DMA_SIZE(1) <=
           AGENTOS_BLK_MEDIA_DMA_OFF(0));
    set.media[1].length[2]=4096; assert(!aos_blk_pci_set_valid(&set));
    set.count=1; assert(aos_blk_pci_set_valid(&set));
    set.count=0; assert(!aos_blk_pci_set_valid(&set));
    set.count=3; assert(!aos_blk_pci_set_valid(&set));
    set.count=2; set.media[1].length[2]=64;
    set.media[1].version=2; assert(!aos_blk_pci_set_valid(&set));
    set.media[1].version=1; set.media[1].reserved=1;
    assert(!aos_blk_pci_set_valid(&set)); set.media[1].reserved=0;
    set.reserved=1; assert(!aos_blk_pci_set_valid(&set)); set.reserved=0;
    set.version=1; assert(!aos_blk_pci_set_valid(&set)); set.version=2;
    set.media[0].offset[0]=4096; assert(!aos_blk_pci_set_valid(&set));
    assert(!aos_blk_pci_set_valid(NULL));
    puts("PASS: independent PCI media descriptions and disjoint register/DMA regions");
    return 0;
}
