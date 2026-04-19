/*
 * contracts/usb-service/interface.h — USB Generic Device Interface
 *
 * // STATUS: PLANNED
 *
 * This header defines the planned contract for the usb-service device service
 * in agentOS.  No implementation exists yet.  This contract specifies the
 * interface that the future USB protection domain must implement and that all
 * guest OS USB stacks must consume.
 *
 * Once implemented, the USB service PD will broker access to USB host
 * controller hardware (EHCI/XHCI via MMIO), presenting individual devices
 * to authorized clients as isolated logical channels.  Direct hardware access
 * by guest OSes or VMMs is prohibited by the device abstraction policy.
 *
 * IPC transport (planned):
 *   - Protected procedure call (PPC) via seL4 Microkit.
 *   - MR0 = opcode (USB_SVC_OP_*)
 *   - MR1..MR6 = arguments (opcode-specific, see per-op comments below)
 *   - Reply: MR0 = status (USB_SVC_ERR_*), MR1..MR6 = result fields
 *
 * Shared-memory transfer buffer (planned):
 *   USB bulk/interrupt data will be transferred via a shared region mapped
 *   into both the USB service PD and the requesting client PD.  The region
 *   size and layout will be defined when the implementation is scoped.
 *   Tentative: 64 KB, subdivided into per-device 4 KB slots.
 *
 * Capability grant (planned):
 *   vm_manager.c will grant a PPC capability to the USB endpoint at guest OS
 *   creation time, together with a shared-memory mapping for bulk transfers.
 *   Devices will be assigned by the USB service during enumeration; the guest
 *   OS must call USB_SVC_OP_ENUMERATE to discover what is available.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ── Interface version ──────────────────────────────────────────────────── */
#define USB_SVC_INTERFACE_VERSION       1

/* ── Geometry / limits (preliminary) ───────────────────────────────────── */
#define USB_SVC_MAX_DEVICES             16u
#define USB_SVC_MAX_ENDPOINTS           8u   /* per device */
#define USB_SVC_SHMEM_SIZE              0x10000u  /* 64 KB (planned) */
#define USB_SVC_SHMEM_SLOT_SIZE         0x1000u   /* 4 KB per device slot */

/* ── USB speed codes ────────────────────────────────────────────────────── */
#define USB_SVC_SPEED_LOW               0u   /* 1.5 Mbit/s  (USB 1.1) */
#define USB_SVC_SPEED_FULL              1u   /* 12 Mbit/s   (USB 1.1) */
#define USB_SVC_SPEED_HIGH              2u   /* 480 Mbit/s  (USB 2.0) */
#define USB_SVC_SPEED_SUPER             3u   /* 5 Gbit/s    (USB 3.0) */

/* ── USB transfer directions ────────────────────────────────────────────── */
#define USB_SVC_DIR_OUT                 0u   /* host → device */
#define USB_SVC_DIR_IN                  1u   /* device → host */

/* ── USB transfer types ─────────────────────────────────────────────────── */
#define USB_SVC_XFER_CONTROL            0u
#define USB_SVC_XFER_ISOCHRONOUS        1u
#define USB_SVC_XFER_BULK               2u
#define USB_SVC_XFER_INTERRUPT          3u

/* ── Opcodes (MR0) ──────────────────────────────────────────────────────── */

/*
 * USB_SVC_OP_ENUMERATE (0xC0)
 * Request an enumeration scan of all attached USB devices.
 *
 * The USB service probes the host controller and returns a bitmask of
 * active device slots.  Callers should call this on startup and after
 * receiving a USB_SVC_OP_ATTACH or USB_SVC_OP_DETACH notification.
 *
 * Request: (none beyond MR0)
 * Reply:
 *   MR0 = status
 *   MR1 = device_bitmask  — bit N set means slot N is occupied
 *   MR2 = device_count    — total active devices
 */
#define USB_SVC_OP_ENUMERATE            0xC0u

/*
 * USB_SVC_OP_ATTACH (0xC1)
 * Claim a USB device slot for exclusive use by the calling PD.
 *
 * The USB service performs address assignment and initial configuration
 * (set_address, get_descriptor) on behalf of the caller.
 *
 * Request:
 *   MR1 = slot            — device slot from the enumeration bitmask
 *   MR2 = caller_pd_id    — PD requesting ownership
 * Reply:
 *   MR0 = status
 *   MR1 = handle          — opaque device handle for subsequent operations
 *   MR2 = vendor_id       — USB idVendor (from device descriptor)
 *   MR3 = product_id      — USB idProduct
 *   MR4 = device_class    — bDeviceClass
 *   MR5 = speed           — USB_SVC_SPEED_*
 */
#define USB_SVC_OP_ATTACH               0xC1u

/*
 * USB_SVC_OP_DETACH (0xC2)
 * Release a previously claimed USB device.
 *
 * Unconfigures the device and returns the slot to the free pool.
 *
 * Request:
 *   MR1 = handle          — device handle returned by USB_SVC_OP_ATTACH
 * Reply:
 *   MR0 = status
 */
#define USB_SVC_OP_DETACH               0xC2u

/*
 * USB_SVC_OP_CONTROL_MSG (0xC3)
 * Issue a USB control transfer (SETUP stage + optional DATA stage).
 *
 * The setup packet (8 bytes) and optional data payload are placed in the
 * shared DMA region at the slot's base offset before this call.  For
 * IN transfers the response data is written back to the same region.
 *
 * Request:
 *   MR1 = handle          — device handle
 *   MR2 = setup_offset    — byte offset in shmem of the 8-byte setup packet
 *   MR3 = data_offset     — byte offset for DATA stage (0 if wLength == 0)
 *   MR4 = data_len        — wLength from setup packet (0 for no data stage)
 *   MR5 = direction       — USB_SVC_DIR_IN or USB_SVC_DIR_OUT
 * Reply:
 *   MR0 = status
 *   MR1 = bytes_xferred   — bytes actually transferred in DATA stage
 */
#define USB_SVC_OP_CONTROL_MSG          0xC3u

/*
 * USB_SVC_OP_BULK_READ (0xC4)
 * Perform a bulk IN transfer from a device endpoint.
 *
 * Data is written into the shared shmem region at the caller's slot offset.
 *
 * Request:
 *   MR1 = handle          — device handle
 *   MR2 = endpoint        — endpoint number (1..USB_SVC_MAX_ENDPOINTS-1)
 *   MR3 = buf_offset      — byte offset in shmem for received data
 *   MR4 = buf_len         — maximum bytes to receive
 * Reply:
 *   MR0 = status
 *   MR1 = bytes_received
 */
#define USB_SVC_OP_BULK_READ            0xC4u

/*
 * USB_SVC_OP_BULK_WRITE (0xC5)
 * Perform a bulk OUT transfer to a device endpoint.
 *
 * The caller places data at buf_offset in shmem before this call.
 *
 * Request:
 *   MR1 = handle          — device handle
 *   MR2 = endpoint        — endpoint number
 *   MR3 = buf_offset      — byte offset in shmem of data to send
 *   MR4 = buf_len         — bytes to send
 * Reply:
 *   MR0 = status
 *   MR1 = bytes_sent
 */
#define USB_SVC_OP_BULK_WRITE           0xC5u

/* ── Error / status codes (MR0 in replies) ──────────────────────────────── */
#define USB_SVC_ERR_OK                  0u   /* success */
#define USB_SVC_ERR_NO_DEVICE           1u   /* no device in specified slot */
#define USB_SVC_ERR_NOT_FOUND           2u   /* handle not valid */
#define USB_SVC_ERR_PERM                3u   /* capability check failed */
#define USB_SVC_ERR_BUSY                4u   /* device claimed by another PD */
#define USB_SVC_ERR_STALL               5u   /* endpoint STALL received */
#define USB_SVC_ERR_TIMEOUT             6u   /* transfer timed out */
#define USB_SVC_ERR_INVAL               7u   /* invalid argument */
#define USB_SVC_ERR_OVERFLOW            8u   /* data too large for buffer */
#define USB_SVC_ERR_UNIMPLEMENTED       99u  /* PLANNED: not yet implemented */

/* ── Device descriptor cached in the shared region ──────────────────────── */
/*
 * After a successful USB_SVC_OP_ATTACH, the USB service writes a
 * usb_svc_device_info_t into the device's shmem slot so the client can
 * inspect it without issuing an additional IPC round-trip.
 */
typedef struct __attribute__((packed)) {
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  speed;           /* USB_SVC_SPEED_* */
    uint8_t  num_configs;
    uint8_t  num_interfaces;
    uint8_t  _reserved[2];
    char     manufacturer[32]; /* UTF-8 (truncated) */
    char     product[32];
    char     serial[32];
} usb_svc_device_info_t;      /* 112 bytes */

/* ── Request / reply structs ─────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;
    uint32_t handle;       /* device handle (or slot for ENUMERATE/ATTACH) */
    uint32_t arg1;         /* endpoint / setup_offset / caller_pd_id */
    uint32_t arg2;         /* buf_offset / data_offset */
    uint32_t arg3;         /* buf_len / data_len */
    uint32_t arg4;         /* direction */
} usb_svc_req_t;

typedef struct __attribute__((packed)) {
    uint32_t status;         /* USB_SVC_ERR_* */
    uint32_t field1;         /* bytes_received / bytes_sent / handle / device_bitmask */
    uint32_t field2;         /* device_count / vendor_id / bytes_xferred */
    uint32_t field3;         /* product_id */
    uint32_t field4;         /* device_class */
    uint32_t field5;         /* speed */
} usb_svc_reply_t;
