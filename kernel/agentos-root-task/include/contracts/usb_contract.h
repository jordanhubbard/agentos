/*
 * USB Device PD IPC Contract
 *
 * The usb_pd owns USB host controller hardware via seL4 device frame
 * capabilities.  It exposes device enumeration and transfer I/O.
 *
 * Channel: CH_USB_PD (see agentos.h)
 * Opcodes: MSG_USB_* (see agentos.h)
 *
 * Invariants:
 *   - MSG_USB_ENUMERATE triggers device enumeration; call once after boot.
 *   - MSG_USB_LIST places usb_dev_info_t[] in usb_shmem.
 *   - MSG_USB_OPEN returns a handle for a specific device index.
 *   - MSG_USB_CONTROL setup packet is placed in usb_shmem before the call.
 *   - MSG_USB_BULK_IN / MSG_USB_BULK_OUT use usb_shmem for data.
 *   - Interrupt and isochronous transfers are not yet supported (Phase 2+).
 *   - A handle is invalidated if the physical device is unplugged; subsequent
 *     calls return USB_ERR_DISCONNECTED.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define USB_PD_CH_CONTROLLER  CH_USB_PD

/* ─── Configuration ──────────────────────────────────────────────────────── */
#define USB_MAX_DEVICES   32u
#define USB_MAX_CLIENTS   16u

/* ─── USB device speed constants ─────────────────────────────────────────── */
#define USB_SPEED_FULL    0  /* 12 Mbps */
#define USB_SPEED_HIGH    1  /* 480 Mbps */
#define USB_SPEED_SUPER   2  /* 5 Gbps */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct usb_req_enumerate {
    /* no fields — triggers enumeration of all connected devices */
};

struct usb_req_list {
    uint32_t max_entries;
};

struct usb_req_open {
    uint32_t dev_index;         /* index from MSG_USB_LIST */
};

struct usb_req_close {
    uint32_t handle;
};

struct usb_req_control {
    uint32_t handle;
    uint32_t setup_len;         /* length of setup packet in usb_shmem (8 bytes) */
    uint32_t data_len;          /* length of data stage in usb_shmem */
    uint32_t flags;             /* USB_CTRL_FLAG_* */
};

#define USB_CTRL_FLAG_IN   (1u << 0)  /* data stage is IN (device → host) */

struct usb_req_bulk_in {
    uint32_t handle;
    uint32_t ep;                /* endpoint address (0x81 = EP1 IN, etc.) */
    uint32_t max;               /* max bytes to receive */
};

struct usb_req_bulk_out {
    uint32_t handle;
    uint32_t ep;                /* endpoint address (0x01 = EP1 OUT, etc.) */
    uint32_t len;               /* bytes in usb_shmem to transmit */
};

struct usb_req_status {
    uint32_t handle;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct usb_reply_enumerate {
    uint32_t ok;
    uint32_t device_count;
};

struct usb_reply_list {
    uint32_t ok;
    uint32_t count;             /* entries written to usb_shmem */
};

struct usb_reply_open {
    uint32_t ok;
    uint32_t handle;
};

struct usb_reply_close {
    uint32_t ok;
};

struct usb_reply_control {
    uint32_t ok;
    uint32_t actual;            /* bytes in data stage */
};

struct usb_reply_bulk_in {
    uint32_t ok;
    uint32_t actual;            /* bytes received into usb_shmem */
};

struct usb_reply_bulk_out {
    uint32_t ok;
    uint32_t actual;            /* bytes actually transmitted */
};

struct usb_reply_status {
    uint32_t ok;
    uint32_t state;             /* USB_STATE_* */
    uint32_t error_code;
    uint32_t speed;             /* USB_SPEED_* */
};

#define USB_STATE_CONNECTED    0
#define USB_STATE_CONFIGURED   1
#define USB_STATE_SUSPENDED    2
#define USB_STATE_ERROR        3

/* ─── Shmem layout: device info entry ───────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t dev_index;
    uint16_t vid;               /* USB Vendor ID */
    uint16_t pid;               /* USB Product ID */
    uint8_t  dev_class;         /* USB device class code */
    uint8_t  speed;             /* USB_SPEED_* */
    uint8_t  _pad[2];
    uint8_t  manufacturer[32];  /* NUL-terminated manufacturer string */
    uint8_t  product[32];       /* NUL-terminated product string */
} usb_dev_info_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum usb_error {
    USB_OK                  = 0,
    USB_ERR_NO_SLOTS        = 1,
    USB_ERR_BAD_HANDLE      = 2,
    USB_ERR_BAD_DEV_INDEX   = 3,
    USB_ERR_DISCONNECTED    = 4,
    USB_ERR_STALL           = 5,  /* USB STALL condition */
    USB_ERR_TIMEOUT         = 6,
    USB_ERR_BAD_EP          = 7,
};
