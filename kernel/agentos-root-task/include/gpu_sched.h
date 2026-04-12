/*
 * gpu_sched.h — GPU Scheduler Protection Domain public interface
 *
 * Extends the GPU scheduler (gpu_sched.c) with virtio-gpu MMIO passthrough
 * support (VFIO-style: direct virtio-MMIO register access from a seL4 PD).
 *
 * IPC opcodes MSG_GPU_SUBMIT / MSG_GPU_STATUS / MSG_GPU_CANCEL are defined
 * in agentos.h.  This header adds the virtio-gpu specific opcode and
 * hardware constants needed for the MMIO passthrough path.
 *
 * virtio-gpu device:
 *   Device ID 16 (0x10) in the virtio device ID registry.
 *   MMIO base virtual address is set by Microkit via setvar_vaddr
 *   (symbol: virtio_gpu_mmio_vaddr).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ── virtio-gpu device identifier ────────────────────────────────────────── */

/*
 * VIRTIO_GPU_DEVICE_ID — virtio spec device ID for GPU (virtio-gpu).
 * See: virtio spec §5.7 (Device ID 16).
 */
#define VIRTIO_GPU_DEVICE_ID    16u

/* ── virtio-MMIO register offsets (same transport as virtio-blk/net) ─────── */
/*
 * These offsets are defined in virtio_blk.h but redefined here for
 * documentation completeness.  Do not include both headers in the same TU.
 */
#ifndef VIRTIO_MMIO_MAGIC_VALUE
#  define VIRTIO_MMIO_MAGIC_VALUE   0x000u
#  define VIRTIO_MMIO_VERSION       0x004u
#  define VIRTIO_MMIO_DEVICE_ID     0x008u
#  define VIRTIO_MMIO_QUEUE_NOTIFY  0x050u
#  define VIRTIO_MMIO_STATUS        0x070u
#  define VIRTIO_MMIO_MAGIC         0x74726976u
#  define VIRTIO_STATUS_ACKNOWLEDGE (1u << 0)
#  define VIRTIO_STATUS_DRIVER      (1u << 1)
#  define VIRTIO_STATUS_DRIVER_OK   (1u << 2)
#  define VIRTIO_STATUS_FEATURES_OK (1u << 3)
#endif

/* ── IPC Opcode: GPU command buffer submission ────────────────────────────── */

/*
 * OP_GPU_SUBMIT_CMD (0xE4) — submit a virtio-gpu command buffer to the device
 *
 *   MR1 = slot_id    (gpu_shmem slot index; identifies which gpu_shmem slot
 *                     holds the GPU tensor/command context)
 *   MR2 = cmd_offset (byte offset into gpu_shmem payload area for the command
 *                     buffer; must be aligned to 64 bytes)
 *   MR3 = cmd_len    (length of the command buffer in bytes)
 *
 *   Reply:
 *   MR0 = GPU_ERR_OK (0) on success; GPU_ERR_INVALID / GPU_ERR_NO_SLOTS on error
 *   MR1 = fence_id   (monotonic fence value; 0 in stub mode)
 *
 *   If virtio-gpu hardware is present (gpu_hw_present == true):
 *     The command buffer at gpu_shmem[cmd_offset..cmd_offset+cmd_len] is written
 *     to the virtio-gpu virtqueue descriptor, the available ring is updated,
 *     and QUEUE_NOTIFY is kicked to wake the device.
 *
 *   If running in stub mode (no hardware):
 *     The opcode is logged with slot_id, cmd_offset, and cmd_len.
 *     Returns GPU_ERR_OK with fence_id = 0.
 */
#define OP_GPU_SUBMIT_CMD   0xE4u

/* ── Function declarations ───────────────────────────────────────────────── */

/*
 * probe_virtio_gpu() — probe the virtio-gpu MMIO region.
 *
 * Called from gpu_sched's init().  Reads the virtio magic, version, and
 * device_id registers.  If device_id == VIRTIO_GPU_DEVICE_ID (16), sets
 * gpu_hw_present = true and performs the minimal virtio initialisation
 * sequence (ACKNOWLEDGE → DRIVER).
 *
 * Pattern identical to probe_virtio_net() in net_server.c.
 */
void probe_virtio_gpu(void);
