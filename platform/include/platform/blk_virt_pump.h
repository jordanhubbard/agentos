#ifndef AOS_PLATFORM_BLK_VIRT_PUMP_H
#define AOS_PLATFORM_BLK_VIRT_PUMP_H

#include <platform/blk_layout.h>

void aos_blk_virt_reset(aos_blk_virt_t *v);

/* Bind client_index's stride inside region. Does not touch buffers. */
void aos_blk_client_bind(uint8_t *region, uint32_t client_index,
                         aos_blk_virt_client_t *out);

/* Zero request/response queues. */
void aos_blk_client_init_queues(aos_blk_virt_client_t *c);

/* Fill storage_info for a RAM disk of disk_blocks transfer units. */
void aos_blk_storage_init(aos_blk_storage_info_t *info, uint32_t disk_blocks);

int aos_blk_virt_add_client(aos_blk_virt_t *v, const aos_blk_virt_client_t *c);

void aos_blk_virt_set_disk(aos_blk_virt_t *v, uint8_t *disk, uint32_t disk_blocks);

/*
 * Serve every pending request against the shared RAM disk.
 * Returns I/O operations that produced a response (including invalid-param).
 */
uint32_t aos_blk_virt_pump(aos_blk_virt_t *v);

#endif /* AOS_PLATFORM_BLK_VIRT_PUMP_H */
