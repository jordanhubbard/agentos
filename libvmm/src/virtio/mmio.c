/*
 * Copyright 2023, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <libvmm/util/util.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/virtio.h>
#include <libvmm/virtio/virtq.h>
#include <libvmm/virtio/gpa.h>

/* Uncomment this to enable debug logging */
// #define DEBUG_MMIO

#if defined(DEBUG_MMIO)
#define LOG_MMIO(...) do{ printf("%s|VIRTIO(MMIO): ", vmm_pd_name); printf(__VA_ARGS__); }while(0)
#else
#define LOG_MMIO(...) do{}while(0)
#endif

// @jade: add some giant comments about this file
// generic virtio mmio emul interface

#define REG_RANGE(r0, r1)   r0 ... (r1 - 1)

int handle_virtio_mmio_set_status_flag(virtio_device_t *dev, uint32_t reg)
{
    int success = 1;

    // we only care about the new status
    dev->regs.Status &= reg;
    reg ^= dev->regs.Status;
    LOG_MMIO("set status flag 0x%x.\n", reg);

    switch (reg) {
    case VIRTIO_CONFIG_S_RESET:
        dev->regs.Status = 0;
        /* Virtio 1.2, 4.2.2.1: reset clears every InterruptStatus bit. */
        dev->regs.InterruptStatus = 0;
        dev->funs->device_reset(dev);
        break;

    case VIRTIO_CONFIG_S_ACKNOWLEDGE:
        // are we following the initialization protocol?
        if (dev->regs.Status == 0) {
            dev->regs.Status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
            // nothing to do from our side (as the virtio device).
        }
        break;

    case VIRTIO_CONFIG_S_DRIVER:
        // are we following the initialization protocol?
        if (dev->regs.Status & VIRTIO_CONFIG_S_ACKNOWLEDGE) {
            dev->regs.Status |= VIRTIO_CONFIG_S_DRIVER;
            // nothing to do from our side (as the virtio device).
        }
        break;

    case VIRTIO_CONFIG_S_FEATURES_OK:
        // are we following the initialization protocol?
        if (dev->regs.Status & VIRTIO_CONFIG_S_DRIVER) {
            // are features OK?
            dev->regs.Status |= (dev->features_happy ? VIRTIO_CONFIG_S_FEATURES_OK : 0);
        }
        break;

    case VIRTIO_CONFIG_S_DRIVER_OK:
        dev->regs.Status |= VIRTIO_CONFIG_S_DRIVER_OK;
        // probably do some san checks here
        break;

    case VIRTIO_CONFIG_S_FAILED:
        LOG_MMIO("received FAILED status from driver, giving up this device.\n");
        break;

    default:
        LOG_VMM_ERR("unknown virtIO MMIO device status 0x%x.\n", reg);
        success = 0;
    }
    return success;
}

bool virtio_mmio_reg_read(virtio_device_t *dev, size_t offset, uint32_t *value)
{
    if (dev == NULL || value == NULL) return false;
    uint32_t reg = 0;
    bool success = true;
    LOG_MMIO("read from 0x%lx\n", offset);

    switch (offset) {
    case REG_RANGE(REG_VIRTIO_MMIO_MAGIC_VALUE, REG_VIRTIO_MMIO_VERSION):
        reg = VIRTIO_MMIO_DEV_MAGIC;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_VERSION, REG_VIRTIO_MMIO_DEVICE_ID):
        reg = VIRTIO_MMIO_DEV_VERSION;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_DEVICE_ID, REG_VIRTIO_MMIO_VENDOR_ID):
        reg = dev->regs.DeviceID;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_VENDOR_ID, REG_VIRTIO_MMIO_DEVICE_FEATURES):
        reg = dev->regs.VendorID;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_DEVICE_FEATURES, REG_VIRTIO_MMIO_DEVICE_FEATURES_SEL):
        success = dev->funs->get_device_features(dev, &reg);
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_NUM_MAX, REG_VIRTIO_MMIO_QUEUE_NUM):
        reg = QUEUE_SIZE;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_READY, REG_VIRTIO_MMIO_QUEUE_NOTIFY):
        if (dev->regs.QueueSel < dev->num_vqs) {
            reg = dev->vqs[dev->regs.QueueSel].ready;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_READY\n", dev->regs.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_INTERRUPT_STATUS, REG_VIRTIO_MMIO_INTERRUPT_ACK):
        reg = dev->regs.InterruptStatus;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_STATUS, REG_VIRTIO_MMIO_QUEUE_DESC_LOW):
        reg = dev->regs.Status;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_CONFIG_GENERATION, REG_VIRTIO_MMIO_CONFIG):
        reg = dev->regs.ConfigGeneration;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_CONFIG, REG_VIRTIO_MMIO_CONFIG + 0x100):
        success = dev->funs->get_device_config(dev, offset - REG_VIRTIO_MMIO_CONFIG, &reg);
        break;
    default:
        LOG_VMM_ERR("unknown virtIO MMIO register read at offset 0x%x\n", offset);
        success = false;
    }

    if (success) *value = reg;

    return success;
}

bool virtio_mmio_reg_write(virtio_device_t *dev, size_t offset, uint32_t data)
{
    if (dev == NULL) return false;
    /* Mapped ring pointers and lengths are immutable until device reset.
     * Reprogramming a ready queue would mix GPAs with translated pointers. */
    if (dev->regs.QueueSel<dev->num_vqs && dev->vqs[dev->regs.QueueSel].ready &&
        ((offset>=REG_VIRTIO_MMIO_QUEUE_NUM && offset<REG_VIRTIO_MMIO_QUEUE_READY) ||
         (offset>=REG_VIRTIO_MMIO_QUEUE_DESC_LOW && offset<REG_VIRTIO_MMIO_CONFIG_GENERATION)))
        return false;
    bool success = true;

    switch (offset) {
    case REG_RANGE(REG_VIRTIO_MMIO_DEVICE_FEATURES_SEL, REG_VIRTIO_MMIO_DRIVER_FEATURES):
        dev->regs.DeviceFeaturesSel = data;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_DRIVER_FEATURES, REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL):
        success = dev->funs->set_driver_features(dev, data);
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL, REG_VIRTIO_MMIO_QUEUE_SEL):
        dev->regs.DriverFeaturesSel = data;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_SEL, REG_VIRTIO_MMIO_QUEUE_NUM_MAX):
        dev->regs.QueueSel = data;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_NUM, REG_VIRTIO_MMIO_QUEUE_READY): {
        if (dev->regs.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            virtq->num = (unsigned int)data;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_NUM\n", dev->regs.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_READY, REG_VIRTIO_MMIO_QUEUE_NOTIFY):
        if (data == 0x1) {
            if (dev->regs.QueueSel >= dev->num_vqs) {
                LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                            "given when accessing REG_VIRTIO_MMIO_QUEUE_READY\n",
                            dev->regs.QueueSel, dev->num_vqs);
                success = false;
                break;
            }
            /*
             * Map QueueDesc/Avail/Used GPAs to HVAs before walking the rings.
             * This fails until the VMM installs a translator with
             * virtio_gpa_set_translate().
             */
            if (!dev->vqs[dev->regs.QueueSel].ready) {
                if (!virtio_queue_map_guest_rings(&dev->vqs[dev->regs.QueueSel].virtq)) {
                    LOG_VMM_ERR("virtq GPA→HVA map failed (QueueSel 0x%x)\n",
                                dev->regs.QueueSel);
                    success = false;
                    break;
                }
            }
            dev->vqs[dev->regs.QueueSel].ready = true;
        }
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_NOTIFY, REG_VIRTIO_MMIO_INTERRUPT_STATUS):
        dev->regs.QueueNotify = data;
        success = dev->funs->queue_notify(dev);
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_INTERRUPT_ACK, REG_VIRTIO_MMIO_STATUS):
        dev->regs.InterruptStatus &= ~data;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_STATUS, REG_VIRTIO_MMIO_QUEUE_DESC_LOW):
        success = handle_virtio_mmio_set_status_flag(dev, data);
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_DESC_LOW, REG_VIRTIO_MMIO_QUEUE_DESC_HIGH): {
        if (dev->regs.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->desc;
            ptr = (ptr & UINT64_C(0xffffffff00000000)) | data;
            virtq->desc = (struct virtq_desc *)ptr;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_DESC_LOW\n", dev->regs.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_DESC_HIGH, REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW): {
        if (dev->regs.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->desc;
            ptr = (ptr & UINT64_C(0xffffffff)) | ((uintptr_t)data << 32);
            virtq->desc = (struct virtq_desc *)ptr;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_DESC_HIGH\n", dev->regs.QueueSel, dev->num_vqs);
            success = false;
        }
    }
    break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW, REG_VIRTIO_MMIO_QUEUE_AVAIL_HIGH): {
        if (dev->regs.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->avail;
            ptr = (ptr & UINT64_C(0xffffffff00000000)) | data;
            virtq->avail = (struct virtq_avail *)ptr;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW\n", dev->regs.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_AVAIL_HIGH, REG_VIRTIO_MMIO_QUEUE_USED_LOW): {
        if (dev->regs.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->avail;
            ptr = (ptr & UINT64_C(0xffffffff)) | ((uintptr_t)data << 32);
            virtq->avail = (struct virtq_avail *)ptr;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_AVAIL_HIGH\n", dev->regs.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_USED_LOW, REG_VIRTIO_MMIO_QUEUE_USED_HIGH): {
        if (dev->regs.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->used;
            ptr = (ptr & UINT64_C(0xffffffff00000000)) | data;
            virtq->used = (struct virtq_used *)ptr;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_USED_LOW\n", dev->regs.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_USED_HIGH, REG_VIRTIO_MMIO_CONFIG_GENERATION): {
        if (dev->regs.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->used;
            ptr = (ptr & UINT64_C(0xffffffff)) | ((uintptr_t)data << 32);
            virtq->used = (struct virtq_used *)ptr;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_USED_HIGH\n", dev->regs.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_CONFIG, REG_VIRTIO_MMIO_CONFIG + 0x100):
        success = dev->funs->set_device_config(dev, offset, data);
        break;
    default:
        LOG_VMM_ERR("unknown virtIO MMIO register write at offset 0x%x\n", offset);
        success = false;
    }

    return success;
}
