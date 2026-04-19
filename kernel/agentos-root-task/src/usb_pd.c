/*
 * usb_pd.c — agentOS Generic USB Protection Domain
 *
 * OS-neutral IPC USB host controller service. Provides ENUMERATE/LIST/OPEN/CLOSE/
 * CONTROL/BULK_IN/BULK_OUT/STATUS over seL4 IPC with shared memory for data.
 * Up to USB_MAX_DEVICES simultaneously tracked; up to USB_MAX_DEVICES open handles.
 *
 * IPC Protocol (caller -> usb_pd, channel CH_USB_PD):
 *   MSG_USB_ENUMERATE (0x1030) — trigger enumeration → MR1=count
 *   MSG_USB_LIST      (0x1031) — device list → shmem, MR1=count
 *   MSG_USB_OPEN      (0x1032) — MR1=dev_index → MR1=handle
 *   MSG_USB_CLOSE     (0x1033) — MR1=handle → ok
 *   MSG_USB_CONTROL   (0x1034) — MR1=handle; setup in shmem → ok
 *   MSG_USB_BULK_IN   (0x1035) — MR1=handle MR2=ep MR3=max → data in shmem, MR1=actual
 *   MSG_USB_BULK_OUT  (0x1036) — MR1=handle MR2=ep MR3=len; data in shmem → ok
 *   MSG_USB_STATUS    (0x1037) — MR1=handle → MR1=state MR2=err
 *
 * Hardware: xHCI or DWC3 USB host controller via device capability.
 *           Actual transfer submission is Phase 2.5 (see TODO below).
 *
 * Priority: 205
 */
#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/usb_contract.h"

/* ── State ───────────────────────────────────────────────────────────────── */
static uint32_t s_device_count;
static uint32_t s_dev_state[USB_MAX_DEVICES];   /* usb_dev_state_t per slot */
static uint32_t s_dev_error[USB_MAX_DEVICES];
static bool     s_handle_open[USB_MAX_DEVICES]; /* handle table parallel to device table */

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    s_device_count = 0;
    for (uint32_t i = 0; i < USB_MAX_DEVICES; i++) {
        s_dev_state[i]   = USB_DEV_DISCONNECTED;
        s_dev_error[i]   = 0;
        s_handle_open[i] = false;
    }
    microkit_dbg_puts("[usb_pd] ready\n");
}

void notified(microkit_channel ch) { (void)ch; }

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch; (void)msginfo;
    uint32_t op     = (uint32_t)microkit_mr_get(0);
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    switch (op) {
    case MSG_USB_ENUMERATE:
        /*
         * TODO Phase 2.5: walk xHCI port status registers, allocate device slots,
         * perform SET_ADDRESS and GET_DESCRIPTOR sequences.
         * For now, report zero devices.
         */
        s_device_count = 0;
        microkit_mr_set(0, USB_OK);
        microkit_mr_set(1, s_device_count);
        return microkit_msginfo_new(0, 2);
    case MSG_USB_LIST:
        /*
         * TODO Phase 2.5: copy usb_device_info_t[] array into usb_shmem.
         * For now, just report count.
         */
        microkit_mr_set(0, USB_OK);
        microkit_mr_set(1, s_device_count);
        return microkit_msginfo_new(0, 2);
    case MSG_USB_OPEN: {
        uint32_t dev_index = handle; /* MR1 = dev_index on OPEN */
        if (dev_index >= USB_MAX_DEVICES || dev_index >= s_device_count) {
            microkit_mr_set(0, USB_ERR_BAD_INDEX);
            return microkit_msginfo_new(0, 1);
        }
        if (s_handle_open[dev_index]) {
            /* Already open — return same handle */
            microkit_mr_set(0, USB_OK);
            microkit_mr_set(1, dev_index);
            return microkit_msginfo_new(0, 2);
        }
        s_handle_open[dev_index] = true;
        s_dev_state[dev_index]   = USB_DEV_OPEN;
        microkit_mr_set(0, USB_OK);
        microkit_mr_set(1, dev_index);
        return microkit_msginfo_new(0, 2);
    }
    case MSG_USB_CLOSE:
        if (handle >= USB_MAX_DEVICES || !s_handle_open[handle]) {
            microkit_mr_set(0, USB_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        s_handle_open[handle] = false;
        s_dev_state[handle]   = USB_DEV_CONNECTED;
        microkit_mr_set(0, USB_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_USB_CONTROL:
        if (handle >= USB_MAX_DEVICES || !s_handle_open[handle]) {
            microkit_mr_set(0, USB_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        /* TODO Phase 2.5: read 8-byte setup packet from usb_shmem offset 0, submit control TRB */
        microkit_mr_set(0, USB_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_USB_BULK_IN: {
        if (handle >= USB_MAX_DEVICES || !s_handle_open[handle]) {
            microkit_mr_set(0, USB_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        /* TODO Phase 2.5: submit IN TRB on endpoint MR2, wait, copy data to usb_shmem */
        microkit_mr_set(0, USB_OK);
        microkit_mr_set(1, 0); /* 0 bytes received until HW path implemented */
        return microkit_msginfo_new(0, 2);
    }
    case MSG_USB_BULK_OUT: {
        if (handle >= USB_MAX_DEVICES || !s_handle_open[handle]) {
            microkit_mr_set(0, USB_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        /* TODO Phase 2.5: read data from usb_shmem, submit OUT TRB on endpoint MR2 */
        microkit_mr_set(0, USB_OK);
        return microkit_msginfo_new(0, 1);
    }
    case MSG_USB_STATUS:
        if (handle >= USB_MAX_DEVICES) {
            microkit_mr_set(0, USB_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        microkit_mr_set(0, USB_OK);
        microkit_mr_set(1, s_dev_state[handle]);
        microkit_mr_set(2, s_dev_error[handle]);
        return microkit_msginfo_new(0, 3);
    default:
        microkit_dbg_puts("[usb_pd] unknown op\n");
        microkit_mr_set(0, USB_ERR_NOT_IMPL);
        return microkit_msginfo_new(0, 1);
    }
}
