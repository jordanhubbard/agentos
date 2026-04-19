/*
 * serial_pd.c — agentOS Generic Serial Protection Domain
 *
 * OS-neutral IPC serial port service. Provides OPEN/CLOSE/READ/WRITE/STATUS/CONFIGURE.
 * After serial_pd reports ready via EventBus, all serial I/O must go through this PD.
 * Falling back to microkit_dbg_puts is permitted only during early boot (before init()).
 *
 * IPC Protocol (caller -> serial_pd, channel CH_SERIAL_PD):
 *   MSG_SERIAL_OPEN      (0x1000) — claim port → client_slot
 *   MSG_SERIAL_CLOSE     (0x1001) — release slot
 *   MSG_SERIAL_WRITE     (0x1002) — MR1=slot MR2=offset MR3=len → MR1=written
 *   MSG_SERIAL_READ      (0x1003) — MR1=slot MR2=max → MR1=avail
 *   MSG_SERIAL_STATUS    (0x1004) — MR1=slot → MR1=baud MR2=rx MR3=tx
 *   MSG_SERIAL_CONFIGURE (0x1005) — MR1=slot MR2=baud MR3=flags → ok
 *
 * Hardware: PL011 UART at base address resolved from device capability.
 *           DMA and IRQ paths are Phase 2.5 (see TODO below).
 *
 * Priority: 220 (above most services, below EventBus)
 */
#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/serial_contract.h"

#define SERIAL_MAX_CLIENTS_INTERNAL  SERIAL_MAX_CLIENTS

/* ── State ───────────────────────────────────────────────────────────────── */
static bool     s_slot_open[SERIAL_MAX_CLIENTS_INTERNAL];
static uint32_t s_rx_count[SERIAL_MAX_CLIENTS_INTERNAL];
static uint32_t s_tx_count[SERIAL_MAX_CLIENTS_INTERNAL];
static uint32_t s_baud[SERIAL_MAX_CLIENTS_INTERNAL];

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    for (uint32_t i = 0; i < SERIAL_MAX_CLIENTS_INTERNAL; i++) {
        s_slot_open[i] = false;
        s_rx_count[i]  = 0;
        s_tx_count[i]  = 0;
        s_baud[i]      = 115200;
    }
    microkit_dbg_puts("[serial_pd] ready\n");
}

void notified(microkit_channel ch) { (void)ch; }

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch; (void)msginfo;
    uint32_t op   = (uint32_t)microkit_mr_get(0);
    uint32_t slot = (uint32_t)microkit_mr_get(1);

    switch (op) {
    case MSG_SERIAL_OPEN: {
        uint32_t found = SERIAL_MAX_CLIENTS_INTERNAL;
        for (uint32_t i = 0; i < SERIAL_MAX_CLIENTS_INTERNAL; i++) {
            if (!s_slot_open[i]) { found = i; break; }
        }
        if (found == SERIAL_MAX_CLIENTS_INTERNAL) {
            microkit_mr_set(0, SERIAL_ERR_NO_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        s_slot_open[found] = true;
        microkit_mr_set(0, SERIAL_OK);
        microkit_mr_set(1, found);
        return microkit_msginfo_new(0, 2);
    }
    case MSG_SERIAL_CLOSE:
        if (slot < SERIAL_MAX_CLIENTS_INTERNAL) s_slot_open[slot] = false;
        microkit_mr_set(0, SERIAL_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_SERIAL_WRITE:
        /* TODO Phase 2.5: DMA transfer from shmem to PL011 TX FIFO */
        if (slot < SERIAL_MAX_CLIENTS_INTERNAL) s_tx_count[slot]++;
        microkit_mr_set(0, SERIAL_OK);
        microkit_mr_set(1, (uint32_t)microkit_mr_get(3)); /* echo len as written */
        return microkit_msginfo_new(0, 2);
    case MSG_SERIAL_READ:
        /* TODO Phase 2.5: DMA transfer from PL011 RX FIFO to shmem */
        if (slot < SERIAL_MAX_CLIENTS_INTERNAL) s_rx_count[slot]++;
        microkit_mr_set(0, SERIAL_OK);
        microkit_mr_set(1, 0); /* no data yet */
        return microkit_msginfo_new(0, 2);
    case MSG_SERIAL_STATUS:
        if (slot >= SERIAL_MAX_CLIENTS_INTERNAL) {
            microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        microkit_mr_set(0, SERIAL_OK);
        microkit_mr_set(1, s_baud[slot]);
        microkit_mr_set(2, s_rx_count[slot]);
        microkit_mr_set(3, s_tx_count[slot]);
        return microkit_msginfo_new(0, 4);
    case MSG_SERIAL_CONFIGURE:
        if (slot < SERIAL_MAX_CLIENTS_INTERNAL)
            s_baud[slot] = (uint32_t)microkit_mr_get(2);
        microkit_mr_set(0, SERIAL_OK);
        return microkit_msginfo_new(0, 1);
    default:
        microkit_dbg_puts("[serial_pd] unknown op\n");
        microkit_mr_set(0, SERIAL_ERR_NOT_IMPL);
        return microkit_msginfo_new(0, 1);
    }
}
