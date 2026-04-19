/*
 * log_drain.c — agentOS Log Drain Protection Domain
 *
 * Pure ring-buffer drain: reads structured log records from per-PD shared
 * memory rings and writes them to the debug UART. No sessions, no modes,
 * no interactive semantics.
 *
 * IPC Protocol (any PD -> log_drain, channel CH_LOG_DRAIN):
 *   OP_LOG_WRITE  (0x01) — MR1=slot, MR2=pd_id: register slot and drain it
 *   OP_LOG_STATUS (0x02) — returns MR1=slot_count, MR2=bytes_drained_total
 *
 * Ring layout (per PD, LOG_DRAIN_RING_SIZE bytes each):
 *   [0..15]    log_drain_ring_t header (magic, pd_id, head, tail)
 *   [16..]     circular character data
 *
 * Wire format is identical to the prior console_mux ring (magic 0xC0DE4D55)
 * so existing PDs need no ring layout changes.
 *
 * Priority: 160 (drain promptly; above workers, below eventbus)
 * Mode: passive
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

#define MAX_DRAIN_SLOTS    16
#define LOG_PREFIX_MAX     20   /* "[pd_id] " prefix */

/* ── State ───────────────────────────────────────────────────────────────── */

uintptr_t log_drain_rings_vaddr;

static uint64_t s_bytes_drained = 0;
static uint32_t s_slot_count    = 0;
static bool     s_slot_seen[MAX_DRAIN_SLOTS];

/* ── Ring accessors ──────────────────────────────────────────────────────── */

static volatile log_drain_ring_t *ring_hdr(uint32_t slot)
{
    return (volatile log_drain_ring_t *)
        (log_drain_rings_vaddr + slot * LOG_DRAIN_RING_SIZE);
}

static volatile uint8_t *ring_data(uint32_t slot)
{
    return (volatile uint8_t *)
        (log_drain_rings_vaddr + slot * LOG_DRAIN_RING_SIZE + LOG_DRAIN_RING_HDR_SZ);
}

/* ── UART helpers ────────────────────────────────────────────────────────── */

static void uart_puts(const char *s)  { microkit_dbg_puts(s); }
static void uart_putc(char c)         { char b[2] = {c, '\0'}; microkit_dbg_puts(b); }

static void uart_u32(uint32_t v)
{
    char buf[12]; int i = 11; buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else { while (v && i > 0) { buf[--i] = '0' + (int)(v % 10); v /= 10; } }
    uart_puts(&buf[i]);
}

/* ── Drain one ring ──────────────────────────────────────────────────────── */

static void drain_slot(uint32_t slot)
{
    if (!log_drain_rings_vaddr) return;

    volatile log_drain_ring_t *hdr = ring_hdr(slot);
    if (hdr->magic != LOG_DRAIN_RING_MAGIC) return;

    volatile uint8_t *buf = ring_data(slot);
    uint32_t head = hdr->head;
    uint32_t tail = hdr->tail;

    if (tail == head) return;

    /* Emit "[pd_id] " prefix at start of each new run */
    uart_puts("["); uart_u32(hdr->pd_id); uart_puts("] ");

    while (tail != head) {
        char c = (char)buf[tail % LOG_DRAIN_DATA_SIZE];
        tail = (tail + 1) % LOG_DRAIN_DATA_SIZE;
        s_bytes_drained++;
        uart_putc(c);
        /* Re-emit prefix after each newline */
        if (c == '\n' && tail != head) {
            uart_puts("["); uart_u32(hdr->pd_id); uart_puts("] ");
        }
    }

    hdr->tail = tail;
}

static void drain_all(void)
{
    for (uint32_t i = 0; i < MAX_DRAIN_SLOTS; i++) {
        if (!s_slot_seen[i]) continue;
        drain_slot(i);
    }
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    for (uint32_t i = 0; i < MAX_DRAIN_SLOTS; i++)
        s_slot_seen[i] = false;

    uart_puts("[log_drain] ready\n");
}

void notified(microkit_channel ch)
{
    (void)ch;
    drain_all();
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch; (void)msginfo;

    uint32_t op   = (uint32_t)microkit_mr_get(0);
    uint32_t slot = (uint32_t)microkit_mr_get(1);
    uint32_t pd_id = (uint32_t)microkit_mr_get(2);

    switch (op) {

    case OP_LOG_WRITE:
        if (slot < MAX_DRAIN_SLOTS) {
            volatile log_drain_ring_t *hdr = ring_hdr(slot);
            if (hdr->magic != LOG_DRAIN_RING_MAGIC) {
                /* First write from this PD — initialise the ring header */
                hdr->pd_id = pd_id;
                hdr->head  = 0;
                hdr->tail  = 0;
                hdr->magic = LOG_DRAIN_RING_MAGIC;
                if (!s_slot_seen[slot]) {
                    s_slot_seen[slot] = true;
                    s_slot_count++;
                }
            }
            drain_slot(slot);
        }
        microkit_mr_set(0, 0);
        break;

    case OP_LOG_STATUS:
        microkit_mr_set(0, 0);
        microkit_mr_set(1, s_slot_count);
        microkit_mr_set(2, (uint32_t)(s_bytes_drained & 0xFFFFFFFF));
        break;

    default:
        microkit_dbg_puts("[log_drain] unknown op\n");
        microkit_mr_set(0, 0xFF);
        break;
    }

    return microkit_msginfo_new(0, 4);
}
