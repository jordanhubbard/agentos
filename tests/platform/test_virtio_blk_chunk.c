#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <libvmm/virtio/block_chunk.h>

#define CELL_SIZE 4096u
#define REGION_CELLS 257u

static void test_large_aligned_request(void)
{
    virtio_blk_chunk_t chunk;
    uint32_t total = 320u * CELL_SIZE;

    assert(virtio_blk_chunk_plan(0u, total, 0u, CELL_SIZE,
                                 REGION_CELLS, &chunk));
    assert(chunk.block_number == 0u);
    assert(chunk.data_offset == 0u);
    assert(chunk.body_bytes == REGION_CELLS * CELL_SIZE);
    assert(chunk.cell_count == REGION_CELLS);

    assert(virtio_blk_chunk_plan(0u, total, chunk.body_bytes, CELL_SIZE,
                                 REGION_CELLS, &chunk));
    assert(chunk.block_number == REGION_CELLS);
    assert(chunk.data_offset == 0u);
    assert(chunk.body_bytes == 63u * CELL_SIZE);
    assert(chunk.cell_count == 63u);
}

static void test_unaligned_request_ends_chunks_on_cell_boundary(void)
{
    virtio_blk_chunk_t chunk;
    uint32_t total = 258u * CELL_SIZE;
    uint32_t first_bytes = REGION_CELLS * CELL_SIZE - 512u;

    assert(virtio_blk_chunk_plan(512u, total, 0u, CELL_SIZE,
                                 REGION_CELLS, &chunk));
    assert(chunk.block_number == 0u);
    assert(chunk.data_offset == 512u);
    assert(chunk.body_bytes == first_bytes);
    assert(chunk.cell_count == REGION_CELLS);

    assert(virtio_blk_chunk_plan(512u, total, first_bytes, CELL_SIZE,
                                 REGION_CELLS, &chunk));
    assert(chunk.block_number == REGION_CELLS);
    assert(chunk.data_offset == 0u);
    assert(chunk.body_bytes == CELL_SIZE + 512u);
    assert(chunk.cell_count == 2u);
}

static void test_invalid_inputs_fail_closed(void)
{
    virtio_blk_chunk_t chunk;

    assert(!virtio_blk_chunk_plan(0u, CELL_SIZE, 0u, 0u,
                                  REGION_CELLS, &chunk));
    assert(!virtio_blk_chunk_plan(0u, CELL_SIZE, 0u, CELL_SIZE,
                                  0u, &chunk));
    assert(!virtio_blk_chunk_plan(0u, CELL_SIZE, CELL_SIZE, CELL_SIZE,
                                  REGION_CELLS, &chunk));
    assert(!virtio_blk_chunk_plan(UINT64_MAX, CELL_SIZE, 1u, CELL_SIZE,
                                  REGION_CELLS, &chunk));
}

static void test_overlap_uses_backend_windows(void)
{
    assert(virtio_blk_requests_overlap(0u, 512u, 512u, 512u,
                                       CELL_SIZE));
    assert(!virtio_blk_requests_overlap(0u, CELL_SIZE, CELL_SIZE, 512u,
                                        CELL_SIZE));
    assert(virtio_blk_requests_overlap(0u, 320u * CELL_SIZE,
                                       319u * CELL_SIZE, CELL_SIZE,
                                       CELL_SIZE));
}

static void test_only_active_writes_block_overlapping_requests(void)
{
    assert(virtio_blk_req_state_is_active_write(
        VIRTIO_BLK_REQ_STATE_WRITING_ALIGNED));
    assert(virtio_blk_req_state_is_active_write(
        VIRTIO_BLK_REQ_STATE_RMW_READING));
    assert(virtio_blk_req_state_is_active_write(
        VIRTIO_BLK_REQ_STATE_RMW_WRITING));
    assert(!virtio_blk_req_state_is_active_write(
        VIRTIO_BLK_REQ_STATE_RMW_QUEUEING));
    assert(!virtio_blk_req_state_is_active_write(
        VIRTIO_BLK_REQ_STATE_READING));
}

int main(void)
{
    test_large_aligned_request();
    test_unaligned_request_ends_chunks_on_cell_boundary();
    test_invalid_inputs_fail_closed();
    test_overlap_uses_backend_windows();
    test_only_active_writes_block_overlapping_requests();
    puts("virtio_blk_chunk: all tests passed");
    return 0;
}
