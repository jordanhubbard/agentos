/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <libvmm/vmm_caps.h>
#include <libvmm/vcpu.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>
#include <libvmm/arch/aarch64/vgic/virq.h>

#if defined(GIC_V2)
#include <libvmm/arch/aarch64/vgic/vgic_v2.h>
#elif defined(GIC_V3)
#include <libvmm/arch/aarch64/vgic/vgic_v3.h>
#else
#error "Unknown GIC version"
#endif

#include <libvmm/arch/aarch64/vgic/vdist.h>

/* The driver expects the VGIC state to be initialised before calling any of the driver functionality. */
extern vgic_t vgic;

#define PPI_VTIMER_IRQ 27

static bool vgic_maintenance_reinject(size_t vcpu_id, int irq)
{
    if (irq != PPI_VTIMER_IRQ) {
        return false;
    }

    /*
     * FreeBSD EOIs before advancing CVAL. Keep the physical VPPI masked and
     * inject one more virtual timer IRQ while the level remains asserted.
     * The subsequent EOI observes the deasserted level and acknowledges the
     * physical VPPI. vdist clears pending when loading the LR, so this
     * reinjection cannot be silently discarded.
     */
    seL4_Word ctl =
        vmm_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CNTV_CTL);
    return (ctl & 0x5u) == 0x5u;
}

bool vgic_flush_pending_irqs(size_t vcpu_id)
{
#if defined(GIC_V2)
    const int group = 0;
#elif defined(GIC_V3)
    const int group = 1;
#else
#error "Unknown GIC version"
#endif
    int idx;

    while ((idx = vgic_find_empty_list_reg(&vgic, vcpu_id)) >= 0) {
        struct virq_handle *virq = vgic_irq_dequeue(&vgic, vcpu_id);
        if (virq == NULL) {
            return true;
        }
        if (!vgic_vcpu_load_list_reg(&vgic, vcpu_id, idx, group, virq)) {
            set_pending(&vgic, virq->virq, false, vcpu_id);
            return false;
        }
        set_pending(&vgic, virq->virq, false, vcpu_id);
    }
    return true;
}

bool vgic_handle_fault_maintenance(size_t vcpu_id)
{
    // @ivanv: reivist, also inconsistency between int and bool
    bool success = true;
    int idx = seL4_GetMR(seL4_VGICMaintenance_IDX);
    static uint64_t maint_count = 0;
    maint_count++;
    bool log_maintenance = maint_count <= 4 || (maint_count % 100000) == 0;
    if (log_maintenance) {
        LOG_VMM("VGICMaintenance #%llu: IDX=%d on vCPU %zu\n",
                (unsigned long long)maint_count, idx, vcpu_id);
    }
    if (idx < 0) {
        /* Drain queued IRQs without dropping one when no LR is available. */
        if (!vgic_flush_pending_irqs(vcpu_id)) {
            LOG_VMM_ERR("vGIC spurious maintenance queue flush failed\n");
        }
        return true;
    }

    // @ivanv: Revisit and make sure it's still correct.
    vgic_vcpu_t *vgic_vcpu = get_vgic_vcpu(&vgic, vcpu_id);
    assert(vgic_vcpu);
    assert((idx >= 0) && (idx < ARRAY_SIZE(vgic_vcpu->lr_shadow)));
    struct virq_handle *slot = &vgic_vcpu->lr_shadow[idx];
    if (log_maintenance) {
        LOG_VMM("VGICMaintenance #%llu: LR[%d] holds IRQ %d (INVALID=%s)\n",
                (unsigned long long)maint_count, idx,
                slot->virq, slot->virq == VIRQ_INVALID ? "YES" : "no");
    }
    assert(slot->virq != VIRQ_INVALID);
    struct virq_handle lr_virq = *slot;
    slot->virq = VIRQ_INVALID;
    slot->ack_fn = NULL;
    slot->ack_data = NULL;
    /* Clear pending */
    LOG_IRQ("Maintenance IRQ %d\n", lr_virq.virq);
    set_pending(&vgic, lr_virq.virq, false, vcpu_id);
    bool reinject = vgic_maintenance_reinject(vcpu_id, lr_virq.virq);
#if defined(GIC_V2)
    int group = 0;
#elif defined(GIC_V3)
    int group = 1;
#else
#error "Unknown GIC version"
#endif
    if (reinject) {
        set_pending(&vgic, lr_virq.virq, true, vcpu_id);
        success = vgic_vcpu_load_list_reg(&vgic, vcpu_id, idx, group, &lr_virq);
        if (success) {
            set_pending(&vgic, lr_virq.virq, false, vcpu_id);
        } else {
            set_pending(&vgic, lr_virq.virq, false, vcpu_id);
            virq_ack(vcpu_id, &lr_virq);
        }
        if (!vgic_flush_pending_irqs(vcpu_id)) {
            LOG_VMM_ERR("vGIC timer maintenance queue flush failed\n");
        }
        return true;
    }
    virq_ack(vcpu_id, &lr_virq);
    success = vgic_flush_pending_irqs(vcpu_id);

    if (!success) {
        LOG_VMM_ERR("vGIC maintenance: load_list_reg failed for idx %d\n", idx);
    }

    return true;
}

bool vgic_handle_fault_dist(size_t vcpu_id, size_t offset, size_t fsr, seL4_UserContext *regs, void *data)
{
    bool success = false;
    if (fault_is_read(fsr)) {
        success = vgic_handle_fault_dist_read(vcpu_id, &vgic, offset, fsr, regs);
        if (!success) {
            LOG_VMM_ERR("vGIC dist read fault at offset 0x%lx failed\n", offset);
        }
    } else {
        success = vgic_handle_fault_dist_write(vcpu_id, &vgic, offset, fsr, regs);
        if (!success) {
            LOG_VMM_ERR("vGIC dist write fault at offset 0x%lx failed\n", offset);
            success = true;
        }
    }

    return success;
}

bool vgic_register_irq(size_t vcpu_id, int virq_num, virq_ack_fn_t ack_fn, void *ack_data)
{
    assert(virq_num >= 0 && virq_num != VIRQ_INVALID);
    struct virq_handle virq = {
        .virq = virq_num,
        .ack_fn = ack_fn,
        .ack_data = ack_data,
    };

    return virq_add(vcpu_id, &vgic, &virq);
}

bool vgic_inject_irq(size_t vcpu_id, int irq)
{
    LOG_IRQ("(vCPU %d) injecting IRQ %d\n", vcpu_id, irq);

    return vgic_dist_set_pending_irq(&vgic, vcpu_id, irq);
}

bool vgic_irq_is_inflight(size_t vcpu_id, int irq)
{
    vgic_vcpu_t *vgic_vcpu = get_vgic_vcpu(&vgic, vcpu_id);
    if (!vgic_vcpu) return false;
    for (int i = 0; i < NUM_LIST_REGS; i++) {
        if (vgic_vcpu->lr_shadow[i].virq == irq) return true;
    }
    return false;
}

bool vgic_irq_is_pending(size_t vcpu_id, int irq)
{
    return is_pending(&vgic, irq, (int)vcpu_id);
}

bool vgic_irq_is_enabled(size_t vcpu_id, int irq)
{
    return is_enabled(&vgic, irq, (int)vcpu_id);
}
