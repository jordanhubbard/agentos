/*
 * serial_pd.c — agentOS Serial I/O Protection Domain
 *
 * Owns the UART hardware exclusively.  No other PD may map the UART
 * device frame.  Guest VMMs and all other PDs reach serial I/O via IPC.
 *
 * Physical layer: ARM PL011 UART (AArch64 boards).
 * Virtual ports: up to SERIAL_MAX_CLIENTS, multiplexed on one physical UART.
 *   - port_id 0 : raw (no banner)
 *   - port_id N : output prefixed with "[portN] "
 *
 * IPC protocol — opcode in MR0, args in MR1..MR3:
 *   MSG_SERIAL_OPEN      MR1=port_id  → MR0=ok  MR1=client_slot
 *   MSG_SERIAL_CLOSE     MR1=slot     → MR0=ok
 *   MSG_SERIAL_WRITE     MR1=slot MR2=len; TX data in serial_shmem → MR0=written
 *   MSG_SERIAL_READ      MR1=slot MR2=max → MR0=count; RX data in serial_shmem
 *   MSG_SERIAL_STATUS    MR1=slot → MR0=baud MR1=rx_cnt MR2=tx_cnt MR3=errs
 *   MSG_SERIAL_CONFIGURE MR1=slot MR2=baud MR3=flags → MR0=ok
 *
 * Priority: 180  (above workers/agents; below eventbus)
 * Mode: passive  (woken by PPC from callers, and UART RX IRQ notification)
 *
 * AGENTS.md Rule 6: This is the generic serial PD.  No guest OS or other PD
 * implements its own UART driver.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/serial_contract.h"

/* ─── PL011 UART register offsets ───────────────────────────────────────── */

#define UART_DR     0x000u
#define UART_RSR    0x004u
#define UART_FR     0x018u
#define UART_IBRD   0x024u
#define UART_FBRD   0x028u
#define UART_LCR_H  0x02Cu
#define UART_CR     0x030u
#define UART_IMSC   0x038u
#define UART_MIS    0x040u
#define UART_ICR    0x044u

#define UART_FR_TXFF  (1u << 5)
#define UART_FR_RXFE  (1u << 4)
#define UART_FR_BUSY  (1u << 3)

#define UART_CR_RXE    (1u << 9)
#define UART_CR_TXE    (1u << 8)
#define UART_CR_UARTEN (1u << 0)

#define UART_LCR_FEN   (1u << 4)
#define UART_LCR_WLEN8 (3u << 5)

#define UART_DR_OE  (1u << 11)
#define UART_DR_FE  (1u << 8)
#define UART_DR_PE  (1u << 9)

/* ─── IRQ notification channel ──────────────────────────────────────────── */

#define CH_UART_IRQ  1u

/* ─── Per-slot RX ring buffer ────────────────────────────────────────────── */

#define RX_BUF_SIZE  512u

typedef struct {
    uint8_t  data[RX_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
} rx_ring_t;

/* ─── Virtual client slot ────────────────────────────────────────────────── */

typedef struct {
    bool      open;
    uint32_t  port_id;
    uint32_t  baud;
    uint32_t  flags;
    uint32_t  rx_count;
    uint32_t  tx_count;
    uint32_t  err_flags;
    rx_ring_t rx;
} serial_client_t;

/* ─── Module state ───────────────────────────────────────────────────────── */

/* Microkit setvar_vaddr — set by system initializer from system description */
uintptr_t uart_mmio_vaddr;
uintptr_t serial_shmem_vaddr;

static serial_client_t clients[SERIAL_MAX_CLIENTS];
static bool            hw_ready = false;

/* ─── MMIO helpers ───────────────────────────────────────────────────────── */

static inline volatile uint32_t *uart_reg(uint32_t off)
{
    return (volatile uint32_t *)(uart_mmio_vaddr + off);
}

static inline uint32_t pl011_rd(uint32_t off)   { return *uart_reg(off); }
static inline void     pl011_wr(uint32_t off, uint32_t v) { *uart_reg(off) = v; }

/* ─── PL011 driver ───────────────────────────────────────────────────────── */

/*
 * Baud divisors for a 24 MHz UARTCLK (QEMU virt / RPI3 default).
 * Formula: IBRD = floor(UARTCLK / (16 * baud))
 *          FBRD = round((UARTCLK / (16 * baud) - IBRD) * 64)
 */
static void pl011_set_baud(uint32_t baud)
{
    uint32_t ibrd, fbrd;

    switch (baud) {
    case 9600:   ibrd = 156; fbrd = 16; break;
    case 38400:  ibrd = 39;  fbrd = 4;  break;
    case 57600:  ibrd = 26;  fbrd = 3;  break;
    default:
    case 115200: ibrd = 13;  fbrd = 1;  break;
    }

    /* Must be done while UART is disabled */
    uint32_t cr = pl011_rd(UART_CR);
    pl011_wr(UART_CR, 0);
    while (pl011_rd(UART_FR) & UART_FR_BUSY) {}

    pl011_wr(UART_IBRD,  ibrd);
    pl011_wr(UART_FBRD,  fbrd);
    pl011_wr(UART_LCR_H, UART_LCR_WLEN8 | UART_LCR_FEN);
    pl011_wr(UART_CR, cr | UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);
}

static void pl011_init(void)
{
    pl011_wr(UART_CR, 0);
    while (pl011_rd(UART_FR) & UART_FR_BUSY) {}

    pl011_wr(UART_IBRD,  13);
    pl011_wr(UART_FBRD,  1);
    pl011_wr(UART_LCR_H, UART_LCR_WLEN8 | UART_LCR_FEN);
    pl011_wr(UART_IMSC,  (1u << 4));  /* RXIM: interrupt on RX */
    pl011_wr(UART_CR,    UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);
}

static void pl011_putc(char c)
{
    while (pl011_rd(UART_FR) & UART_FR_TXFF) {}
    pl011_wr(UART_DR, (uint32_t)(uint8_t)c);
}

static void pl011_puts(const char *s)
{
    for (; *s; s++) pl011_putc(*s);
}

static void pl011_put_banner(uint32_t port_id)
{
    if (port_id == 0) return;

    char buf[] = "[port0] ";
    uint32_t n = port_id;
    if (n > 9) {
        buf[5] = (char)('0' + (n / 10) % 10);
        buf[6] = (char)('0' + (n % 10));
        buf[7] = ']';
        buf[8] = ' ';
        buf[9] = '\0';
        pl011_puts(buf + 4);  /* "[NN] " */
    } else {
        buf[5] = (char)('0' + n);
        pl011_puts(buf);
    }
}

/* ─── RX ring helpers ────────────────────────────────────────────────────── */

static void rx_push(rx_ring_t *r, uint8_t c)
{
    uint32_t next = (r->head + 1) % RX_BUF_SIZE;
    if (next != r->tail) {
        r->data[r->head] = c;
        r->head = next;
    }
    /* Drop byte on full ring — caller must read promptly */
}

static uint32_t rx_drain(rx_ring_t *r, uint8_t *dst, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && r->tail != r->head) {
        dst[n++] = r->data[r->tail];
        r->tail  = (r->tail + 1) % RX_BUF_SIZE;
    }
    return n;
}

/* ─── Hardware RX poll ───────────────────────────────────────────────────── */

static void poll_hw_rx(void)
{
    if (!hw_ready) return;

    while (!(pl011_rd(UART_FR) & UART_FR_RXFE)) {
        uint32_t dr  = pl011_rd(UART_DR);
        uint8_t  c   = (uint8_t)(dr & 0xFF);
        uint32_t err = 0;

        if (dr & UART_DR_OE) err |= SERIAL_ERR_OVERRUN;
        if (dr & UART_DR_FE) err |= SERIAL_ERR_FRAMING;
        if (dr & UART_DR_PE) err |= SERIAL_ERR_PARITY;

        /* Deliver to every open slot on port_id 0 (default physical port) */
        for (uint32_t i = 0; i < SERIAL_MAX_CLIENTS; i++) {
            if (!clients[i].open || clients[i].port_id != 0) continue;
            rx_push(&clients[i].rx, c);
            clients[i].rx_count++;
            if (err) clients[i].err_flags |= err;
        }
    }

    pl011_wr(UART_ICR, 0x7FFu);  /* clear all interrupt flags */
}

/* ─── IPC handlers ───────────────────────────────────────────────────────── */

static void handle_open(void)
{
    uint32_t port_id = (uint32_t)microkit_mr_get(1);

    for (uint32_t i = 0; i < SERIAL_MAX_CLIENTS; i++) {
        if (clients[i].open) continue;

        clients[i].open      = true;
        clients[i].port_id   = port_id;
        clients[i].baud      = 115200;
        clients[i].flags     = 0;
        clients[i].rx_count  = 0;
        clients[i].tx_count  = 0;
        clients[i].err_flags = 0;
        clients[i].rx.head   = 0;
        clients[i].rx.tail   = 0;

        microkit_mr_set(0, SERIAL_OK);
        microkit_mr_set(1, i);
        return;
    }

    microkit_mr_set(0, SERIAL_ERR_NO_SLOTS);
    microkit_mr_set(1, 0);
}

static void handle_close(void)
{
    uint32_t slot = (uint32_t)microkit_mr_get(1);

    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
        return;
    }

    clients[slot].open = false;
    microkit_mr_set(0, SERIAL_OK);
}

static void handle_write(void)
{
    uint32_t slot = (uint32_t)microkit_mr_get(1);
    uint32_t len  = (uint32_t)microkit_mr_get(2);

    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
        microkit_mr_set(1, 0);
        return;
    }

    if (len > SERIAL_MAX_WRITE_BYTES) len = SERIAL_MAX_WRITE_BYTES;

    const uint8_t *data = (const uint8_t *)serial_shmem_vaddr;

    if (hw_ready) {
        pl011_put_banner(clients[slot].port_id);
        for (uint32_t i = 0; i < len; i++)
            pl011_putc((char)data[i]);
    }

    clients[slot].tx_count += len;
    microkit_mr_set(0, SERIAL_OK);
    microkit_mr_set(1, len);
}

static void handle_read(void)
{
    uint32_t slot = (uint32_t)microkit_mr_get(1);
    uint32_t max  = (uint32_t)microkit_mr_get(2);

    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
        microkit_mr_set(1, 0);
        return;
    }

    poll_hw_rx();

    if (max > RX_BUF_SIZE) max = RX_BUF_SIZE;

    uint8_t *shmem = (uint8_t *)serial_shmem_vaddr;
    uint32_t n     = rx_drain(&clients[slot].rx, shmem, max);

    microkit_mr_set(0, SERIAL_OK);
    microkit_mr_set(1, n);
}

static void handle_status(void)
{
    uint32_t slot = (uint32_t)microkit_mr_get(1);

    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
        return;
    }

    microkit_mr_set(0, SERIAL_OK);
    microkit_mr_set(1, clients[slot].baud);
    microkit_mr_set(2, clients[slot].rx_count);
    microkit_mr_set(3, clients[slot].tx_count);
    microkit_mr_set(4, clients[slot].err_flags);
}

static void handle_configure(void)
{
    uint32_t slot  = (uint32_t)microkit_mr_get(1);
    uint32_t baud  = (uint32_t)microkit_mr_get(2);
    uint32_t flags = (uint32_t)microkit_mr_get(3);

    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
        return;
    }

    if (baud != 9600 && baud != 38400 && baud != 57600 && baud != 115200) {
        microkit_mr_set(0, SERIAL_ERR_BAD_BAUD);
        return;
    }

    clients[slot].baud  = baud;
    clients[slot].flags = flags;

    /* Reprogram hardware only for the "master" port (port_id == 0).
     * Virtual ports share the physical baud rate. */
    if (clients[slot].port_id == 0 && hw_ready)
        pl011_set_baud(baud);

    microkit_mr_set(0, SERIAL_OK);
}

/* ─── Microkit entry points ──────────────────────────────────────────────── */

void init(void)
{
    microkit_dbg_puts("[serial_pd] starting — agentOS serial I/O service\n");

    for (uint32_t i = 0; i < SERIAL_MAX_CLIENTS; i++)
        clients[i].open = false;

    if (uart_mmio_vaddr) {
        pl011_init();
        hw_ready = true;
        pl011_puts("[serial_pd] PL011 UART ready — 115200 8N1\n");
    } else {
        microkit_dbg_puts("[serial_pd] WARNING: uart_mmio_vaddr not mapped "
                          "(x86 or misconfigured manifest)\n");
    }
}

void notified(microkit_channel ch)
{
    if (ch == CH_UART_IRQ)
        poll_hw_rx();
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch;
    (void)msginfo;

    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
    case MSG_SERIAL_OPEN:      handle_open();      break;
    case MSG_SERIAL_CLOSE:     handle_close();     break;
    case MSG_SERIAL_WRITE:     handle_write();     break;
    case MSG_SERIAL_READ:      handle_read();      break;
    case MSG_SERIAL_STATUS:    handle_status();    break;
    case MSG_SERIAL_CONFIGURE: handle_configure(); break;
    default:
        microkit_dbg_puts("[serial_pd] unknown opcode\n");
        microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
        break;
    }

    return microkit_msginfo_new(0, 5);
}
