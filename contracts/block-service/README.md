# BlockService — Block Storage Service Contract

## Overview

BlockService provides sector-granularity read/write access to canonical
agentOS-owned virtio-blk media.  It mediates all block storage I/O for guest
OSes and VMMs through a capability-gated IPC interface, preventing direct
virtio queue access.

Features:
- Sector-granularity READ and WRITE via DMA shared memory
- Cache FLUSH for write-through durability
- GEOMETRY query (sector count, sector size, max transfer)
- TRIM / discard for SSD wear leveling
- Explicit media selection with independent queues and DMA windows

## Status

**IMPLEMENTED.**  The concrete implementation is split between:
- `kernel/agentos-root-task/src/virtio_blk.c` — virtio-blk driver
- `kernel/agentos-root-task/src/vfs_server.c` — VFS layer

## Protection Domain

BlockService runs as the `virtio_blk` passive PD.  Guest OSes receive a
PPC capability to the block-service endpoint and a read-write mapping of
`blk_dma_shmem` at guest OS creation time via `vm_manager.c`.

The root task records the physical address of the shared frame in metadata.
The service translates each media's queue and DMA offsets from that physical
base; it never assumes virtual and physical addresses are identical.

## Media isolation

Interface version 2 requires `media_id` on every operation. Ubuntu installation
media is `BLK_SVC_MEDIA_UBUNTU_INSTALL`; FreeBSD installation media is
`BLK_SVC_MEDIA_FREEBSD_INSTALL`. The canonical service owns both host
transports and gives each one separate queue and DMA storage. Guest VMMs receive
only the service endpoint and shared data mapping, never host MMIO capabilities.

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
