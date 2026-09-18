/* Copyright 2023, UNSW (ABN 57 195 873 179)
 * SPDX-License-Identifier: BSD-2-Clause
 * AArch64 fault decoding for the shared virtio MMIO register implementation. */
#include <libvmm/vmm_caps.h>
#include <libvmm/guest.h>
#include <libvmm/virq.h>
#include <libvmm/virtio/virtio.h>
#include <libvmm/arch/aarch64/fault.h>

bool virtio_mmio_fault_handle(size_t vcpu_id, size_t offset, size_t fsr,
                             seL4_UserContext *regs, void *data)
{
    (void)vcpu_id;
    virtio_device_t *dev = data;
    uint32_t mask = fault_get_data_mask(offset, fsr);
    if (fault_is_read(fsr)) {
        uint32_t value;
        if (!virtio_mmio_reg_read(dev, offset, &value)) return false;
        fault_emulate_write(regs, offset, fsr, value & mask);
        return true;
    }
    return virtio_mmio_reg_write(dev, offset,
        fault_get_data(regs, fsr) & (mask >> ((offset & 3u) * 8u)));
}

static void virtio_virq_default_ack(size_t vcpu_id, int irq, void *cookie)
{
    virtio_device_t *dev = cookie;
    /* Preserve the level when a new completion arrives between ACK and EOI. */
    if (dev->regs.InterruptStatus && !virq_inject_vcpu(vcpu_id, irq)) {
        LOG_VMM_ERR("could not reassert virtio MMIO IRQ %d\n", irq);
    }
}

bool virtio_mmio_register_device(virtio_device_t *dev, uintptr_t region_base,
                                 uintptr_t region_size, size_t virq)
{
    assert(dev->transport_type == VIRTIO_TRANSPORT_MMIO);
    if (!fault_register_vm_exception_handler(region_base, region_size,
                                             &virtio_mmio_fault_handle, dev)) {
        LOG_VMM_ERR("Could not register virtual memory fault handler for "
                    "virtIO region [0x%lx..0x%lx)\n", region_base, region_base + region_size);
        return false;
    }
    bool success = virq_register(GUEST_BOOT_VCPU_ID, virq, &virtio_virq_default_ack, dev);
    assert(success);
    return success;
}
