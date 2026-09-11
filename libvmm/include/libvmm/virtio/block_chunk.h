/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pure request-chunk planner shared by the VirtIO block backend and host
 * tests. It deliberately has no seL4 or sDDF dependencies.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct virtio_blk_chunk {
    uint32_t block_number;
    uint32_t data_offset;
    uint32_t body_bytes;
    uint16_t cell_count;
} virtio_blk_chunk_t;

static inline bool virtio_blk_requests_overlap(uint64_t request_byte_offset1,
                                               uint32_t body_bytes1,
                                               uint64_t request_byte_offset2,
                                               uint32_t body_bytes2,
                                               uint32_t transfer_size)
{
    if (body_bytes1 == 0u || body_bytes2 == 0u || transfer_size == 0u) {
        return false;
    }
    if (body_bytes1 - 1u > UINT64_MAX - request_byte_offset1
        || body_bytes2 - 1u > UINT64_MAX - request_byte_offset2) {
        return true;
    }

    uint64_t start_block1 = request_byte_offset1 / transfer_size;
    uint64_t end_block1 = (request_byte_offset1 + body_bytes1 - 1u)
                        / transfer_size;
    uint64_t start_block2 = request_byte_offset2 / transfer_size;
    uint64_t end_block2 = (request_byte_offset2 + body_bytes2 - 1u)
                        / transfer_size;

    return start_block1 <= end_block2 && start_block2 <= end_block1;
}

static inline bool virtio_blk_chunk_plan(uint64_t request_byte_offset,
                                         uint32_t total_body_bytes,
                                         uint32_t completed_body_bytes,
                                         uint32_t transfer_size,
                                         uint16_t data_region_cells,
                                         virtio_blk_chunk_t *chunk)
{
    if (!chunk || transfer_size == 0u || data_region_cells == 0u
        || total_body_bytes == 0u
        || completed_body_bytes >= total_body_bytes
        || completed_body_bytes > UINT64_MAX - request_byte_offset) {
        return false;
    }

    uint64_t byte_offset = request_byte_offset + completed_body_bytes;
    uint64_t block_number = byte_offset / transfer_size;
    uint32_t data_offset = (uint32_t)(byte_offset % transfer_size);
    uint64_t chunk_capacity = (uint64_t)data_region_cells * transfer_size
                            - data_offset;
    uint64_t remaining = (uint64_t)total_body_bytes - completed_body_bytes;
    uint64_t body_bytes = remaining < chunk_capacity ? remaining : chunk_capacity;
    uint64_t cell_count = (data_offset + body_bytes + transfer_size - 1u)
                        / transfer_size;

    if (block_number > UINT32_MAX || body_bytes == 0u
        || body_bytes > UINT32_MAX || cell_count == 0u
        || cell_count > data_region_cells || cell_count > UINT16_MAX) {
        return false;
    }

    chunk->block_number = (uint32_t)block_number;
    chunk->data_offset = data_offset;
    chunk->body_bytes = (uint32_t)body_bytes;
    chunk->cell_count = (uint16_t)cell_count;
    return true;
}
