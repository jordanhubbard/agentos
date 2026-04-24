/*
 * serial_pd.c — agentOS Serial I/O Protection Domain  [E5-S7: raw seL4 IPC]
 *
 * Owns the UART hardware exclusively.  No other PD may map the UART
 * device frame.  Guest VMMs and all other PDs reach serial I/O via IPC.
 *
 * Physical layer: ARM PL011 UART (AArch64 boards).
 * Virtual ports: up to SERIAL_MAX_CLIENTS, multiplexed on one physical UART.
 *   - port_id 0 : raw (no banner)
 *   - port_id N : output prefixed with "[portN] "
 *
 * IPC protocol (sel4_server_t dispatch, opcode in req->opcode):
 *   MSG_SERIAL_OPEN      data[0..3]=port_id  → rep: data[0..3]=ok  data[4..7]=slot
 *   MSG_SERIAL_CLOSE     data[0..3]=slot     → rep: data[0..3]=ok
 *   MSG_SERIAL_WRITE     data[0..3]=slot data[4..7]=len; TX data in shmem → data[0..3]=written
 *   MSG_SERIAL_READ      data[0..3]=slot data[4..7]=max → data[0..3]=count; RX data in shmem
 *   MSG_SERIAL_STATUS    data[0..3]=slot → data[0..3]=baud data[4..7]=rx data[8..11]=tx data[12..15]=errs
 *   MSG_SERIAL_CONFIGURE data[0..3]=slot data[4..7]=baud data[8..11]=flags → data[0..3]=ok
 *
 * Entry point:
 *   void serial_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
 *
 * Priority: 180  (above workers/agents; below eventbus)
 * Mode: passive  (woken by IPC from callers)
 *
 * AGENTS.md Rule 6: This is the generic serial PD.  No guest OS or other PD
 * implements its own UART driver.
 *
 * Bugs fixed in this migration (E5-S7):
 *   - OP_SERIAL_OPEN reply: now correctly returns MR0=ok, MR1=slot_id via
 *     sel4_server_t rep->data layout (was relying on microkit_mr_set which
 *     placed results in the wrong layout for the raw seL4 reply path).
 *   - microkit_dbg_puts replaced with seL4_DebugPutChar loop.
 *   - Channel-based dispatch removed; nameserver registration added.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* ── Conditional compilation ─────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST
/*
 * Host-side test build: provide minimal type stubs so this file compiles
 * without seL4 or Microkit headers.  The test file provides framework.h
 * (which defines microkit_mr_set/get) before including this unit.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef unsigned long      seL4_CPtr;
typedef unsigned long long sel4_badge_t;

typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[48];
} sel4_msg_t;

#define SEL4_ERR_OK          0u
#define SEL4_ERR_INVALID_OP  1u
#define SEL4_ERR_BAD_ARG     4u
#define SEL4_ERR_NO_MEM      5u

typedef uint32_t (*sel4_handler_fn)(sel4_badge_t badge,
                                     const sel4_msg_t *req,
                                     sel4_msg_t *rep,
                                     void *ctx);
#define SEL4_SERVER_MAX_HANDLERS 32u
typedef struct {
    struct {
        uint32_t        opcode;
        sel4_handler_fn fn;
        void           *ctx;
    } handlers[SEL4_SERVER_MAX_HANDLERS];
    uint32_t  handler_count;
    seL4_CPtr ep;
} sel4_server_t;

static inline void sel4_server_init(sel4_server_t *srv, seL4_CPtr ep)
{
    srv->handler_count = 0;
    srv->ep            = ep;
    for (uint32_t i = 0; i < SEL4_SERVER_MAX_HANDLERS; i++) {
        srv->handlers[i].opcode = 0;
        srv->handlers[i].fn     = (sel4_handler_fn)0;
        srv->handlers[i].ctx    = (void *)0;
    }
}
static inline int sel4_server_register(sel4_server_t *srv, uint32_t opcode,
                                        sel4_handler_fn fn, void *ctx)
{
    if (srv->handler_count >= SEL4_SERVER_MAX_HANDLERS) return -1;
    srv->handlers[srv->handler_count].opcode = opcode;
    srv->handlers[srv->handler_count].fn     = fn;
    srv->handlers[srv->handler_count].ctx    = ctx;
    srv->handler_count++;
    return 0;
}
static inline uint32_t sel4_server_dispatch(sel4_server_t *srv,
                                             sel4_badge_t badge,
                                             const sel4_msg_t *req,
                                             sel4_msg_t *rep)
{
    for (uint32_t i = 0; i < srv->handler_count; i++) {
        if (srv->handlers[i].opcode == req->opcode) {
            uint32_t rc = srv->handlers[i].fn(badge, req, rep,
                                               srv->handlers[i].ctx);
            rep->opcode = rc;
            return rc;
        }
    }
    rep->opcode = SEL4_ERR_INVALID_OP;
    rep->length = 0;
    return SEL4_ERR_INVALID_OP;
}
static inline void sel4_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    (void)ep; (void)req;
    rep->opcode = 0;
    rep->length = 0;
}
static inline void seL4_DebugPutChar(char c) { (void)c; }

#else /* !AGENTOS_TEST_HOST — production build */

#include <stdint.h>
#include <stdbool.h>
#include "sel4_server.h"    /* sel4_server_t, sel4_server_init/register/run */
#include "sel4_client.h"    /* sel4_client_t, sel4_client_call */
#include "sel4_ipc.h"       /* sel4_msg_t, sel4_badge_t, SEL4_ERR_* */
#include <sel4/sel4.h>      /* seL4_DebugPutChar */

#endif /* AGENTOS_TEST_HOST */

/* ── Contract opcodes ────────────────────────────────────────────────────── */

#ifndef MSG_SERIAL_OPEN
#define MSG_SERIAL_OPEN       0x2001u
#define MSG_SERIAL_CLOSE      0x2002u
#define MSG_SERIAL_WRITE      0x2003u
#define MSG_SERIAL_READ       0x2004u
#define MSG_SERIAL_STATUS     0x2005u
#define MSG_SERIAL_CONFIGURE  0x2006u
#endif

#ifndef SERIAL_OK
#define SERIAL_OK             0u
#define SERIAL_ERR_NO_SLOTS   1u
#define SERIAL_ERR_BAD_SLOT   3u
#define SERIAL_ERR_BAD_BAUD   4u
#endif

#ifndef SERIAL_MAX_CLIENTS
#define SERIAL_MAX_CLIENTS    8u
#endif

#ifndef SERIAL_MAX_WRITE_BYTES
#define SERIAL_MAX_WRITE_BYTES 256u
#endif

#ifndef SERIAL_ERR_OVERRUN
#define SERIAL_ERR_OVERRUN (1u << 0)
#define SERIAL_ERR_FRAMING (1u << 1)
#define SERIAL_ERR_PARITY  (1u << 2)
#endif

/* Nameserver opcode */
#ifndef OP_NS_REGISTER
#define OP_NS_REGISTER 0xD0u
#endif
#ifndef NS_OK
#define NS_OK 0u
#endif
#ifndef NS_NAME_MAX
#define NS_NAME_MAX 32
#endif

/* ── PL011 UART register offsets ────────────────────────────────────────── */

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

/* ── Per-slot RX ring buffer ─────────────────────────────────────────────── */

#define RX_BUF_SIZE  512u

typedef struct {
    uint8_t  data[RX_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
} rx_ring_t;

/* ── Virtual client slot ─────────────────────────────────────────────────── */

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

/* ── Module state ────────────────────────────────────────────────────────── */

/*
 * uart_mmio_vaddr — UART MMIO virtual address.
 * In production set by the root task before calling serial_pd_main().
 * In test builds set directly by the test harness (or left 0).
 */
uintptr_t uart_mmio_vaddr;

/*
 * serial_shmem_vaddr — shared memory for TX/RX data transfer.
 * Set by the root task in production; in test builds, set to a static buffer.
 */
uintptr_t serial_shmem_vaddr;

static serial_client_t clients[SERIAL_MAX_CLIENTS];
static bool            hw_ready = false;

/* Server instance */
static sel4_server_t g_srv;

/* ── Data-field helpers (little-endian uint32 in sel4_msg_t.data) ─────────── */

static inline uint32_t data_rd32(const uint8_t *d, int off)
{
    return (uint32_t)d[off    ]
         | ((uint32_t)d[off+1] <<  8)
         | ((uint32_t)d[off+2] << 16)
         | ((uint32_t)d[off+3] << 24);
}

static inline void data_wr32(uint8_t *d, int off, uint32_t v)
{
    d[off  ] = (uint8_t)(v      );
    d[off+1] = (uint8_t)(v >>  8);
    d[off+2] = (uint8_t)(v >> 16);
    d[off+3] = (uint8_t)(v >> 24);
}

/* ── Debug output ────────────────────────────────────────────────────────── */

static void dbg_puts(const char *s)
{
    for (; *s; s++)
        seL4_DebugPutChar(*s);
}

/* ── MMIO helpers ────────────────────────────────────────────────────────── */

static inline volatile uint32_t *uart_reg(uint32_t off)
{
    return (volatile uint32_t *)(uart_mmio_vaddr + off);
}

static inline uint32_t pl011_rd(uint32_t off)   { return *uart_reg(off); }
static inline void     pl011_wr(uint32_t off, uint32_t v) { *uart_reg(off) = v; }

/* ── PL011 driver ────────────────────────────────────────────────────────── */

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

    char buf[11] = "[port00] ";
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

/* ── RX ring helpers ─────────────────────────────────────────────────────── */

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

/* ── Hardware RX poll ────────────────────────────────────────────────────── */

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

/* ── IPC handlers ────────────────────────────────────────────────────────── */

/*
 * handle_open — MSG_SERIAL_OPEN
 *
 * Request data layout:
 *   data[0..3] = port_id
 *
 * Reply data layout (fix for pre-existing bug: was using microkit_mr_set
 * which did not map to the correct raw IPC reply fields):
 *   data[0..3] = SERIAL_OK (0) or error code
 *   data[4..7] = slot_id (only valid when data[0..3] == SERIAL_OK)
 */
static uint32_t handle_open(sel4_badge_t badge, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t port_id = data_rd32(req->data, 0);

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

        data_wr32(rep->data, 0, SERIAL_OK);
        data_wr32(rep->data, 4, i);          /* slot_id */
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    data_wr32(rep->data, 0, SERIAL_ERR_NO_SLOTS);
    data_wr32(rep->data, 4, 0u);
    rep->length = 8;
    return SEL4_ERR_NO_MEM;
}

/*
 * handle_close — MSG_SERIAL_CLOSE
 *
 * Request data layout:
 *   data[0..3] = slot
 *
 * Reply data layout:
 *   data[0..3] = SERIAL_OK or error code
 */
static uint32_t handle_close(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t slot = data_rd32(req->data, 0);

    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        data_wr32(rep->data, 0, SERIAL_ERR_BAD_SLOT);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    clients[slot].open = false;
    data_wr32(rep->data, 0, SERIAL_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/*
 * handle_write — MSG_SERIAL_WRITE
 *
 * Request data layout:
 *   data[0..3] = slot
 *   data[4..7] = len (bytes in serial_shmem)
 *
 * Reply data layout:
 *   data[0..3] = SERIAL_OK or error code
 *   data[4..7] = bytes written
 */
static uint32_t handle_write(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t slot = data_rd32(req->data, 0);
    uint32_t len  = data_rd32(req->data, 4);

    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        data_wr32(rep->data, 0, SERIAL_ERR_BAD_SLOT);
        data_wr32(rep->data, 4, 0u);
        rep->length = 8;
        return SEL4_ERR_BAD_ARG;
    }

    if (len > SERIAL_MAX_WRITE_BYTES) len = SERIAL_MAX_WRITE_BYTES;

    const uint8_t *data = (const uint8_t *)serial_shmem_vaddr;

    if (hw_ready && serial_shmem_vaddr) {
        pl011_put_banner(clients[slot].port_id);
        for (uint32_t i = 0; i < len; i++)
            pl011_putc((char)data[i]);
    }

    clients[slot].tx_count += len;
    data_wr32(rep->data, 0, SERIAL_OK);
    data_wr32(rep->data, 4, len);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/*
 * handle_read — MSG_SERIAL_READ
 *
 * Request data layout:
 *   data[0..3] = slot
 *   data[4..7] = max bytes to read
 *
 * Reply data layout:
 *   data[0..3] = SERIAL_OK or error code
 *   data[4..7] = count (bytes placed in serial_shmem)
 */
static uint32_t handle_read(sel4_badge_t badge, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t slot = data_rd32(req->data, 0);
    uint32_t max  = data_rd32(req->data, 4);

    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        data_wr32(rep->data, 0, SERIAL_ERR_BAD_SLOT);
        data_wr32(rep->data, 4, 0u);
        rep->length = 8;
        return SEL4_ERR_BAD_ARG;
    }

    poll_hw_rx();

    if (max > RX_BUF_SIZE) max = RX_BUF_SIZE;

    uint8_t *shmem = (uint8_t *)serial_shmem_vaddr;
    uint32_t n     = 0;
    if (shmem) n = rx_drain(&clients[slot].rx, shmem, max);

    data_wr32(rep->data, 0, SERIAL_OK);
    data_wr32(rep->data, 4, n);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/*
 * handle_status — MSG_SERIAL_STATUS
 *
 * Request data layout:
 *   data[0..3] = slot
 *
 * Reply data layout:
 *   data[0..3]  = SERIAL_OK or error
 *   data[4..7]  = baud
 *   data[8..11] = rx_count
 *   data[12..15] = tx_count
 *   data[16..19] = err_flags
 */
static uint32_t handle_status(sel4_badge_t badge, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t slot = data_rd32(req->data, 0);

    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        data_wr32(rep->data, 0, SERIAL_ERR_BAD_SLOT);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    data_wr32(rep->data,  0, SERIAL_OK);
    data_wr32(rep->data,  4, clients[slot].baud);
    data_wr32(rep->data,  8, clients[slot].rx_count);
    data_wr32(rep->data, 12, clients[slot].tx_count);
    data_wr32(rep->data, 16, clients[slot].err_flags);
    rep->length = 20;
    return SEL4_ERR_OK;
}

/*
 * handle_configure — MSG_SERIAL_CONFIGURE
 *
 * Request data layout:
 *   data[0..3] = slot
 *   data[4..7] = baud
 *   data[8..11] = flags
 *
 * Reply data layout:
 *   data[0..3] = SERIAL_OK or error
 */
static uint32_t handle_configure(sel4_badge_t badge, const sel4_msg_t *req,
                                   sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t slot  = data_rd32(req->data, 0);
    uint32_t baud  = data_rd32(req->data, 4);
    uint32_t flags = data_rd32(req->data, 8);

    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        data_wr32(rep->data, 0, SERIAL_ERR_BAD_SLOT);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    if (baud != 9600 && baud != 38400 && baud != 57600 && baud != 115200) {
        data_wr32(rep->data, 0, SERIAL_ERR_BAD_BAUD);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    clients[slot].baud  = baud;
    clients[slot].flags = flags;

    /* Reprogram hardware only for the "master" port (port_id == 0).
     * Virtual ports share the physical baud rate. */
    if (clients[slot].port_id == 0 && hw_ready)
        pl011_set_baud(baud);

    data_wr32(rep->data, 0, SERIAL_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── Nameserver self-registration ────────────────────────────────────────── */

static void register_with_nameserver(seL4_CPtr ns_ep)
{
    if (!ns_ep) return;

    /*
     * OP_NS_REGISTER request data layout (matching nameserver.c handle_register):
     *   data[0..3]   = channel_id  (0 — serial_pd uses nameserver for discovery)
     *   data[4..7]   = pd_id       (serial_pd trace PD id)
     *   data[8..11]  = cap_classes (0 — serial is a device service)
     *   data[12..15] = version     (1)
     *   data[16..47] = name        (NS_NAME_MAX = 32 bytes, "serial")
     */
    sel4_msg_t req, rep;
    req.opcode = OP_NS_REGISTER;

    data_wr32(req.data,  0, 0u);    /* channel_id */
    data_wr32(req.data,  4, 16u);   /* pd_id = serial_pd */
    data_wr32(req.data,  8, 0u);    /* cap_classes */
    data_wr32(req.data, 12, 1u);    /* version */

    /* Copy service name "serial" into data[16..47] */
    const char *name = "serial";
    int i = 0;
    for (; name[i] && (16 + i) < 48; i++)
        req.data[16 + i] = (uint8_t)name[i];
    for (; (16 + i) < 48; i++)
        req.data[16 + i] = 0;

    req.length = 48;

    sel4_call(ns_ep, &req, &rep);
    /* Ignore return — if nameserver is offline, continue */
}

/* ── Test-host entry points ──────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

/*
 * serial_pd_test_init — reset all state and register handlers.
 *
 * Called by the test harness before each group of tests.
 */
void serial_pd_test_init(void)
{
    hw_ready = false;

    for (uint32_t i = 0; i < SERIAL_MAX_CLIENTS; i++) {
        clients[i].open      = false;
        clients[i].port_id   = 0;
        clients[i].baud      = 115200;
        clients[i].flags     = 0;
        clients[i].rx_count  = 0;
        clients[i].tx_count  = 0;
        clients[i].err_flags = 0;
        clients[i].rx.head   = 0;
        clients[i].rx.tail   = 0;
    }

    sel4_server_init(&g_srv, 0 /* ep unused in tests */);
    sel4_server_register(&g_srv, MSG_SERIAL_OPEN,      handle_open,      (void *)0);
    sel4_server_register(&g_srv, MSG_SERIAL_CLOSE,     handle_close,     (void *)0);
    sel4_server_register(&g_srv, MSG_SERIAL_WRITE,     handle_write,     (void *)0);
    sel4_server_register(&g_srv, MSG_SERIAL_READ,      handle_read,      (void *)0);
    sel4_server_register(&g_srv, MSG_SERIAL_STATUS,    handle_status,    (void *)0);
    sel4_server_register(&g_srv, MSG_SERIAL_CONFIGURE, handle_configure, (void *)0);
}

/*
 * serial_pd_dispatch_one — exercise one IPC round-trip through the
 * sel4_server dispatch machinery without seL4.
 */
uint32_t serial_pd_dispatch_one(sel4_badge_t badge,
                                 const sel4_msg_t *req,
                                 sel4_msg_t *rep)
{
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

#else /* !AGENTOS_TEST_HOST — production build */

/*
 * serial_pd_main — production entry point called by the root task boot
 * dispatcher.
 *
 * my_ep: listen endpoint capability (seL4 endpoint cap slot).
 * ns_ep: nameserver endpoint (0 = nameserver not yet available).
 *
 * This function NEVER RETURNS.
 */
void serial_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    dbg_puts("[serial_pd] starting — agentOS serial I/O service\n");

    /* Zero client table */
    for (uint32_t i = 0; i < SERIAL_MAX_CLIENTS; i++) {
        clients[i].open      = false;
        clients[i].port_id   = 0;
        clients[i].baud      = 115200;
        clients[i].flags     = 0;
        clients[i].rx_count  = 0;
        clients[i].tx_count  = 0;
        clients[i].err_flags = 0;
        clients[i].rx.head   = 0;
        clients[i].rx.tail   = 0;
    }

    /* Initialise PL011 UART hardware if MMIO is mapped */
    if (uart_mmio_vaddr) {
        pl011_init();
        hw_ready = true;
        pl011_puts("[serial_pd] PL011 UART ready — 115200 8N1\n");
    } else {
        dbg_puts("[serial_pd] WARNING: uart_mmio_vaddr not mapped "
                 "(x86 or misconfigured manifest)\n");
    }

    /* Self-register with nameserver so other PDs can discover "serial" */
    register_with_nameserver(ns_ep);

    dbg_puts("[serial_pd] ready — waiting for IPC\n");

    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, MSG_SERIAL_OPEN,      handle_open,      (void *)0);
    sel4_server_register(&g_srv, MSG_SERIAL_CLOSE,     handle_close,     (void *)0);
    sel4_server_register(&g_srv, MSG_SERIAL_WRITE,     handle_write,     (void *)0);
    sel4_server_register(&g_srv, MSG_SERIAL_READ,      handle_read,      (void *)0);
    sel4_server_register(&g_srv, MSG_SERIAL_STATUS,    handle_status,    (void *)0);
    sel4_server_register(&g_srv, MSG_SERIAL_CONFIGURE, handle_configure, (void *)0);

    /* Enter the recv/dispatch/reply loop — never returns */
    sel4_server_run(&g_srv);
}

#endif /* AGENTOS_TEST_HOST */
