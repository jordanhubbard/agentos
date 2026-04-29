/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/vmm_caps.h>
#include <libvmm/guest.h>
#include <libvmm/virq.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>

/* Maps seL4_CPtr cap values (used as cookies) with registered vIRQ */
int virq_passthrough_map[MAX_PASSTHROUGH_IRQ] = {-1};

#define SGI_RESCHEDULE_IRQ  0
#define SGI_FUNC_CALL       1
#define PPI_VTIMER_IRQ      27

__attribute__((weak)) bool agentos_vppi_defer_ack(size_t vcpu_id, int irq)
{
    (void)vcpu_id;
    (void)irq;
    return false;
}

static void vppi_event_ack(size_t vcpu_id, int irq, void *cookie)
{
    (void)cookie;
    if (irq == PPI_VTIMER_IRQ && agentos_vppi_defer_ack(vcpu_id, irq)) {
        return;
    }
    vmm_vcpu_arm_ack_vppi(vcpu_id, irq);
}

static void sgi_ack(size_t vcpu_id, int irq, void *cookie) {}

bool virq_controller_init()
{
    bool success;

    vgic_init();
#if defined(GIC_V2)
    LOG_VMM("initialised virtual GICv2 (distributor: 0x%lx)\n", GIC_DIST_PADDR);
#elif defined(GIC_V3)
    LOG_VMM("initialised virtual GICv3 (distributor: 0x%lx, redistributor: 0x%lx)\n", GIC_DIST_PADDR, GIC_REDIST_PADDR);
#else
#error "Unsupported GIC version"
#endif

    /* Register the fault handler */
    success = fault_register_vm_exception_handler(GIC_DIST_PADDR, GIC_DIST_SIZE, vgic_handle_fault_dist, NULL);
    if (!success) {
        LOG_VMM_ERR("Failed to register fault handler for GIC distributor region\n");
        return false;
    }
#if defined(GIC_V3)
    success = fault_register_vm_exception_handler(GIC_REDIST_PADDR, GIC_REDIST_TOTAL_SIZE, vgic_handle_fault_redist,
                                                  NULL);
    if (!success) {
        LOG_VMM_ERR("Failed to register fault handler for GIC redistributor region\n");
        return false;
    }
#endif

    for (int vcpu = 0; vcpu < GUEST_NUM_VCPUS; vcpu++) {
        success = vgic_register_irq(vcpu, PPI_VTIMER_IRQ, &vppi_event_ack, NULL);
        if (!success) {
            LOG_VMM_ERR("Failed to register vCPU virtual timer IRQ: 0x%lx\n", PPI_VTIMER_IRQ);
            return false;
        }
        success = vgic_register_irq(vcpu, SGI_RESCHEDULE_IRQ, &sgi_ack, NULL);
        if (!success) {
            LOG_VMM_ERR("Failed to register vCPU SGI 0 IRQ");
            return false;
        }
        success = vgic_register_irq(vcpu, SGI_FUNC_CALL, &sgi_ack, NULL);
        if (!success) {
            LOG_VMM_ERR("Failed to register vCPU SGI 1 IRQ");
            return false;
        }
    }

    return true;
}

bool virq_inject_vcpu(size_t vcpu_id, int irq)
{
    return vgic_inject_irq(vcpu_id, irq);
}

bool virq_inject(int irq)
{
    return vgic_inject_irq(GUEST_BOOT_VCPU_ID, irq);
}

bool virq_register(size_t vcpu_id, size_t virq_num, virq_ack_fn_t ack_fn, void *ack_data)
{
    return vgic_register_irq(vcpu_id, virq_num, ack_fn, ack_data);
}

static void virq_passthrough_ack(size_t vcpu_id, int irq, void *cookie)
{
    /* We are down-casting to seL4_CPtr so must first cast to size_t */
    vmm_irq_ack((seL4_CPtr)(size_t)cookie);
}

bool virq_register_passthrough(size_t vcpu_id, size_t irq, seL4_CPtr irq_cap)
{
    assert(irq_cap != 0);
    if (irq_cap == 0) {
        LOG_VMM_ERR("Invalid cap given '0x%lx' for passthrough vIRQ 0x%lx\n", irq_cap, irq);
        return false;
    }

    LOG_VMM("Register passthrough vIRQ 0x%lx on vCPU 0x%lx (IRQ cap: 0x%lx)\n", irq, vcpu_id, irq_cap);
    virq_passthrough_map[irq_cap] = irq;

    bool success = virq_register(GUEST_BOOT_VCPU_ID, irq, &virq_passthrough_ack, (void *)(size_t)irq_cap);
    assert(success);
    if (!success) {
        LOG_VMM_ERR("Failed to register passthrough vIRQ %d\n", irq);
        return false;
    }

    return true;
}

bool virq_handle_passthrough(seL4_CPtr irq_cap)
{
    assert(virq_passthrough_map[irq_cap] >= 0);
    if (virq_passthrough_map[irq_cap] < 0) {
        LOG_VMM_ERR("attempted to handle invalid passthrough IRQ cap 0x%lx\n", irq_cap);
        return false;
    }

    bool success = vgic_inject_irq(GUEST_BOOT_VCPU_ID, virq_passthrough_map[irq_cap]);
    if (!success) {
        LOG_VMM_ERR("could not inject passthrough vIRQ 0x%lx, dropped on vCPU 0x%lx\n", virq_passthrough_map[irq_cap],
                    GUEST_BOOT_VCPU_ID);
        return false;
    }

    return true;
}
