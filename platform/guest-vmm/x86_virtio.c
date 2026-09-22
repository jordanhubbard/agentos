#include <platform/x86_virtio.h>
#include <libvmm/virtio/virtio.h>
#include <libvmm/virtio/gpa.h>
#include <libvmm/virq.h>
#include <libvmm/guest.h>

static aos_x86_ioapic_t *controller;
static uint8_t *guest_ram;
static size_t guest_ram_size;
static virtio_device_t *devices[AOS_X86_VIRTIO_SLOTS];

void aos_x86_virtio_retire(void)
{
    virtio_gpa_set_translate(NULL);
    controller = NULL;
    guest_ram = NULL;
    guest_ram_size = 0u;
    for (unsigned i = 0; i < AOS_X86_VIRTIO_SLOTS; i++) devices[i] = NULL;
}

static void *translate(uint64_t gpa, size_t length)
{
    if (!guest_ram || gpa > guest_ram_size || length > guest_ram_size-gpa)
        return NULL;
    return guest_ram+(size_t)gpa;
}

bool aos_x86_virtio_init(aos_x86_ioapic_t *ioapic, void *ram, size_t ram_size)
{
    if (controller || !ioapic || !ram || !ram_size ||
        ((uintptr_t)ram & 4095u) || (ram_size & 4095u) ||
        ram_size > AOS_X86_VIRTIO_BASE || ram_size > UINTPTR_MAX-(uintptr_t)ram)
        return false;
    controller=ioapic;
    guest_ram=ram;
    guest_ram_size=ram_size;
    virtio_gpa_set_translate(translate);
    return true;
}

static int slot_at(uint64_t gpa)
{
    if (gpa < AOS_X86_VIRTIO_BASE ||
        gpa-AOS_X86_VIRTIO_BASE >= AOS_X86_VIRTIO_SLOTS*AOS_X86_VIRTIO_STRIDE)
        return -1;
    return (int)((gpa-AOS_X86_VIRTIO_BASE)/AOS_X86_VIRTIO_STRIDE);
}

bool virtio_mmio_register_device(virtio_device_t *dev, uintptr_t base,
                                 uintptr_t size, size_t virq)
{
    int slot=slot_at(base);
    if (!controller || !dev || slot < 0 || devices[slot] ||
        (base & (AOS_X86_VIRTIO_STRIDE-1u)) || size != AOS_X86_VIRTIO_STRIDE ||
        virq != AOS_X86_VIRTIO_GSI_BASE+(unsigned)slot || dev->virq != virq ||
        dev->transport_type != VIRTIO_TRANSPORT_MMIO || !dev->vqs ||
        !dev->num_vqs || !dev->funs || !dev->funs->device_reset ||
        !dev->funs->get_device_features || !dev->funs->set_driver_features ||
        !dev->funs->get_device_config || !dev->funs->set_device_config ||
        !dev->funs->queue_notify)
        return false;
    for (unsigned i=0; i<AOS_X86_VIRTIO_SLOTS; i++)
        if (devices[i]==dev) return false;
    devices[slot]=dev;
    return aos_x86_ioapic_set_irq(controller, (unsigned)virq,
                                 dev->regs.InterruptStatus != 0);
}

bool aos_x86_virtio_contains(uint64_t gpa)
{
    int slot=slot_at(gpa);
    return controller && slot >= 0 && devices[slot];
}

bool virq_inject(int irq)
{
    if (!controller || irq < (int)AOS_X86_VIRTIO_GSI_BASE ||
        irq >= (int)(AOS_X86_VIRTIO_GSI_BASE+AOS_X86_VIRTIO_SLOTS))
        return false;
    virtio_device_t *dev=devices[irq-AOS_X86_VIRTIO_GSI_BASE];
    return dev && aos_x86_ioapic_set_irq(controller, (unsigned)irq,
                                        dev->regs.InterruptStatus != 0);
}

bool virq_inject_vcpu(size_t vcpu_id, int irq)
{
    return vcpu_id == GUEST_BOOT_VCPU_ID && virq_inject(irq);
}

bool aos_x86_virtio_access(uint64_t gpa, unsigned width, bool write, uint32_t *value)
{
    if (!value || !aos_x86_virtio_contains(gpa) ||
        (width != 1u && width != 2u && width != 4u) || (gpa & (width-1u)))
        return false;
    unsigned offset=(unsigned)((gpa-AOS_X86_VIRTIO_BASE) % AOS_X86_VIRTIO_STRIDE);
    if (offset >= REG_VIRTIO_MMIO_CONFIG+0x100u ||
        (width != 4u && (write || offset < REG_VIRTIO_MMIO_CONFIG)))
        return false;
    virtio_device_t *dev=devices[slot_at(gpa)];
    if (write) {
        bool ok=virtio_mmio_reg_write(dev, offset, *value);
        /* Device ACK/reset can lower the line. LAPIC EOI alone cannot. */
        bool irq=virq_inject((int)dev->virq);
        return ok && irq;
    }
    uint32_t word;
    if (!virtio_mmio_reg_read(dev, offset & ~3u, &word)) return false;
    uint32_t mask=width == 4u ? UINT32_MAX : (1u << (width*8u))-1u;
    *value=(word >> ((offset & 3u)*8u)) & mask;
    return true;
}
