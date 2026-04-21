# BlockService — Block Storage Service Contract

## Overview

BlockService provides sector-granularity read/write access to the virtio-blk
device.  It mediates all block storage I/O for guest OSes and VMMs through a
capability-gated IPC interface, preventing direct virtio queue access.

Features:
- Sector-granularity READ and WRITE via DMA shared memory
- Cache FLUSH for write-through durability
- GEOMETRY query (sector count, sector size, max transfer)
- TRIM / discard for SSD wear leveling

## Status

**IMPLEMENTED.**  The concrete implementation is split between:
- `kernel/agentos-root-task/src/virtio_blk.c` — virtio-blk driver
- `kernel/agentos-root-task/src/vfs_server.c` — VFS layer

## Protection Domain

BlockService runs as the `virtio_blk` passive PD.  Guest OSes receive a
PPC capability to the block-service endpoint and a read-write mapping of
`blk_dma_shmem` (32KB) at guest OS creation time via `vm_manager.c`.

The physical address of `blk_dma_shmem` equals its virtual address (fixed_mr)
so the virtio device can DMA directly without IOMMU translation.

## Operations

| Opcode | Description |
|--------|-------------|
| `BLK_SVC_OP_READ_BLOCK`  | Read sectors into DMA shmem |
| `BLK_SVC_OP_WRITE_BLOCK` | Write sectors from DMA shmem |
| `BLK_SVC_OP_FLUSH`       | Flush write cache |
| `BLK_SVC_OP_GEOMETRY`    | Query device geometry |
| `BLK_SVC_OP_TRIM`        | Discard sectors (SSD TRIM) |
| `BLK_SVC_OP_STATUS`      | Device health and statistics |

## Source Files

- `contracts/block-service/interface.h` — canonical IPC contract
- `kernel/agentos-root-task/src/virtio_blk.c` — driver implementation
- `kernel/agentos-root-task/include/virtio_blk.h` — virtio constants
