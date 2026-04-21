/*
 * irq_pd.c — agentOS Generic IRQ Protection Domain
 *
 * OS-neutral IPC IRQ routing service. Provides REGISTER/UNREGISTER/ACKNOWLEDGE/
 * MASK/UNMASK/STATUS over seL4 IPC. Registered IRQs are forwarded to caller-specified
 * microkit channels via microkit_notify when they fire.
 *
 * IPC Protocol (caller -> irq_pd, channel CH_IRQ_PD):
 *   MSG_IRQ_REGISTER    (0x1050) — MR1=irq_num MR2=notify_ch → ok
 *   MSG_IRQ_UNREGISTER  (0x1051) — MR1=irq_num → ok
 *   MSG_IRQ_ACKNOWLEDGE (0x1052) — MR1=irq_num → ok
 *   MSG_IRQ_MASK        (0x1053) — MR1=irq_num → ok
 *   MSG_IRQ_UNMASK      (0x1054) — MR1=irq_num → ok
 *   MSG_IRQ_STATUS      (0x1055) — MR1=irq_num → MR1=masked MR2=pending MR3=count
 *
 * Hardware: ARM GIC-400 (GICv2) or GIC-600 (GICv3) via seL4 IRQ capability.
 *           seL4_IRQHandler_SetNotification and seL4_IRQHandler_Ack are Phase 2.5.
 *
 * Priority: 230 (above all device PDs)
 */
#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/irq_contract.h"

/* ── State ───────────────────────────────────────────────────────────────── */
static bool     s_registered[IRQ_MAX_IRQS];
static bool     s_masked[IRQ_MAX_IRQS];
static bool     s_pending[IRQ_MAX_IRQS];
static uint32_t s_notify_ch[IRQ_MAX_IRQS];
static uint32_t s_count[IRQ_MAX_IRQS];

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    for (uint32_t i = 0; i < IRQ_MAX_IRQS; i++) {
        s_registered[i] = false;
        s_masked[i]     = true; /* IRQs default to masked until explicitly unmasked */
        s_pending[i]    = false;
        s_notify_ch[i]  = 0;
        s_count[i]      = 0;
    }
    microkit_dbg_puts("[irq_pd] ready\n");
}

void notified(microkit_channel ch)
{
    /*
     * A notification on channel `ch` means a hardware IRQ fired and was
     * delivered to this PD by the seL4 IRQ handler capability. Find the
     * registered IRQ mapped to this channel, mark it pending, forward
     * the notification to the registered caller channel, and acknowledge
     * the IRQ via the seL4 IRQ handler capability.
     *
     * TODO Phase 2.5: maintain a ch -> irq_num reverse mapping.
     *                 Call seL4_IRQHandler_Ack(irq_handler_cap[irq_num]).
     */
    for (uint32_t i = 0; i < IRQ_MAX_IRQS; i++) {
        if (s_registered[i] && !s_masked[i] && (microkit_channel)s_notify_ch[i] == ch) {
            s_count[i]++;
            s_pending[i] = true;
            microkit_notify((microkit_channel)s_notify_ch[i]);
            /* TODO Phase 2.5: seL4_IRQHandler_Ack(irq_cap[i]); */
            break;
        }
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch; (void)msginfo;
    uint32_t op      = (uint32_t)microkit_mr_get(0);
    uint32_t irq_num = (uint32_t)microkit_mr_get(1);

    /* Validate irq_num for all ops that use it */
    if (op != MSG_IRQ_REGISTER && irq_num >= IRQ_MAX_IRQS) {
        microkit_mr_set(0, IRQ_ERR_BAD_IRQ);
        return microkit_msginfo_new(0, 1);
    }

    switch (op) {
    case MSG_IRQ_REGISTER: {
        if (irq_num >= IRQ_MAX_IRQS) {
            microkit_mr_set(0, IRQ_ERR_BAD_IRQ);
            return microkit_msginfo_new(0, 1);
        }
        if (s_registered[irq_num]) {
            microkit_mr_set(0, IRQ_ERR_ALREADY_REG);
            return microkit_msginfo_new(0, 1);
        }
        uint32_t notify_ch = (uint32_t)microkit_mr_get(2);
        s_registered[irq_num] = true;
        s_masked[irq_num]     = true; /* start masked; caller calls UNMASK when ready */
        s_notify_ch[irq_num]  = notify_ch;
        s_count[irq_num]      = 0;
        s_pending[irq_num]    = false;
        /* TODO Phase 2.5: seL4_IRQHandler_SetNotification(irq_cap[irq_num], notification); */
        microkit_mr_set(0, IRQ_OK);
        return microkit_msginfo_new(0, 1);
    }
    case MSG_IRQ_UNREGISTER:
        if (!s_registered[irq_num]) {
            microkit_mr_set(0, IRQ_ERR_BAD_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        s_masked[irq_num]     = true;
        s_registered[irq_num] = false;
        /* TODO Phase 2.5: seL4_IRQHandler_Clear(irq_cap[irq_num]); */
        microkit_mr_set(0, IRQ_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_IRQ_ACKNOWLEDGE:
        if (!s_registered[irq_num]) {
            microkit_mr_set(0, IRQ_ERR_BAD_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        s_pending[irq_num] = false;
        /* TODO Phase 2.5: seL4_IRQHandler_Ack(irq_cap[irq_num]); */
        microkit_mr_set(0, IRQ_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_IRQ_MASK:
        if (!s_registered[irq_num]) {
            microkit_mr_set(0, IRQ_ERR_BAD_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        s_masked[irq_num] = true;
        /* TODO Phase 2.5: program GIC to mask SPI irq_num */
        microkit_mr_set(0, IRQ_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_IRQ_UNMASK:
        if (!s_registered[irq_num]) {
            microkit_mr_set(0, IRQ_ERR_BAD_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        s_masked[irq_num] = false;
        /* TODO Phase 2.5: program GIC to unmask SPI irq_num */
        microkit_mr_set(0, IRQ_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_IRQ_STATUS:
        if (!s_registered[irq_num]) {
            microkit_mr_set(0, IRQ_ERR_BAD_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        microkit_mr_set(0, IRQ_OK);
        microkit_mr_set(1, s_masked[irq_num]  ? 1u : 0u);
        microkit_mr_set(2, s_pending[irq_num] ? 1u : 0u);
        microkit_mr_set(3, s_count[irq_num]);
        return microkit_msginfo_new(0, 4);
    default:
        microkit_dbg_puts("[irq_pd] unknown op\n");
        microkit_mr_set(0, IRQ_ERR_NOT_IMPL);
        return microkit_msginfo_new(0, 1);
    }
}
