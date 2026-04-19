#pragma once
/* USB_PD contract — version 1
 * PD: usb_pd | Source: src/usb_pd.c | Channel: CH_USB_PD (64) from controller
 * Provides OS-neutral IPC USB host controller access. Enumerate, open, and transfer.
 */
#include <stdint.h>
#include <stdbool.h>

#define USB_PD_CONTRACT_VERSION 1

/* ── Channel ─────────────────────────────────────────────────────────────── */
#define CH_USB_PD  64u

/* ── Opcodes (from agentos_msg_tag_t) ───────────────────────────────────── */
#define USB_OP_ENUMERATE  0x1030u
#define USB_OP_LIST       0x1031u
#define USB_OP_OPEN       0x1032u
#define USB_OP_CLOSE      0x1033u
#define USB_OP_CONTROL    0x1034u
#define USB_OP_BULK_IN    0x1035u
#define USB_OP_BULK_OUT   0x1036u
#define USB_OP_STATUS     0x1037u

#define USB_MAX_DEVICES   8u
#define USB_BULK_MAX      512u  /* max bytes per bulk transfer */

/* ── USB device state ────────────────────────────────────────────────────── */
typedef enum {
    USB_DEV_DISCONNECTED = 0,
    USB_DEV_CONNECTED    = 1,
    USB_DEV_OPEN         = 2,
    USB_DEV_ERROR        = 3,
} usb_dev_state_t;

/* ── Request structs ─────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t op;            /* USB_OP_ENUMERATE */
} usb_req_enumerate_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* USB_OP_LIST */
} usb_req_list_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* USB_OP_OPEN */
    uint32_t dev_index;     /* index from last enumeration, 0-based */
} usb_req_open_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* USB_OP_CLOSE */
    uint32_t handle;
} usb_req_close_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* USB_OP_CONTROL */
    uint32_t handle;
    /* setup packet (8 bytes) placed in usb_shmem at offset 0 by caller */
} usb_req_control_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* USB_OP_BULK_IN */
    uint32_t handle;
    uint32_t endpoint;      /* endpoint number */
    uint32_t max_len;       /* max bytes to receive, max USB_BULK_MAX */
} usb_req_bulk_in_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* USB_OP_BULK_OUT */
    uint32_t handle;
    uint32_t endpoint;      /* endpoint number */
    uint32_t len;           /* bytes in usb_shmem to send, max USB_BULK_MAX */
} usb_req_bulk_out_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* USB_OP_STATUS */
    uint32_t handle;
} usb_req_status_t;

/* ── Reply structs ───────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t result;        /* 0 = ok */
    uint32_t device_count;  /* number of devices found */
} usb_reply_enumerate_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t handle;        /* assigned handle */
} usb_reply_open_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t actual_len;    /* bytes received into usb_shmem */
} usb_reply_bulk_in_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t state;         /* usb_dev_state_t */
    uint32_t error_code;    /* last USB error code */
} usb_reply_status_t;

/* ── USB device descriptor in shmem (for USB_OP_LIST) ───────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t vid;
    uint16_t pid;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint8_t  state;         /* usb_dev_state_t */
    uint8_t  serial[16];    /* null-terminated ASCII serial string */
} usb_device_info_t;

/* ── Error codes ─────────────────────────────────────────────────────────── */
typedef enum {
    USB_OK               = 0,
    USB_ERR_NO_HANDLE    = 1,  /* no free handle slots */
    USB_ERR_BAD_HANDLE   = 2,  /* invalid handle */
    USB_ERR_BAD_INDEX    = 3,  /* dev_index out of range */
    USB_ERR_HW           = 4,  /* hardware / host controller error */
    USB_ERR_NOT_IMPL     = 5,  /* operation not yet implemented */
    USB_ERR_STALL        = 6,  /* endpoint stalled */
    USB_ERR_TIMEOUT      = 7,  /* transfer timed out */
} usb_error_t;

/* ── Invariants ──────────────────────────────────────────────────────────
 * - USB_OP_ENUMERATE must be called before USB_OP_LIST or USB_OP_OPEN.
 * - usb_shmem is shared; only one in-flight transfer per handle at a time.
 * - CONTROL setup packet (8 bytes) must be pre-loaded into usb_shmem by caller.
 * - BULK_IN result data is written by usb_pd into usb_shmem; caller reads after reply.
 */
