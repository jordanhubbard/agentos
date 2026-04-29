/*
 * cc_pd.c — agentOS Command-and-Control Protection Domain
 *
 * Receives binary-framed requests from external callers (agentctl,
 * agentos_gui) via a VirtIO MMIO serial port, which QEMU bridges to
 * build/cc_pd.sock on the host.  Routes each call to the appropriate
 * service PD via seL4 IPC and returns the binary-framed reply.
 *
 * Transport:  VirtIO MMIO serial, virtio-mmio-bus.2 (PA 0x0A000400).
 *   QEMU args: -chardev socket,id=cc_pd_char,path=build/cc_pd.sock,...
 *              -device virtio-serial-device,bus=virtio-mmio-bus.2,id=vser0
 *              -device virtserialport,bus=vser0.0,chardev=cc_pd_char,name=cc.0
 *   Wire frame (both directions): 4112 bytes
 *     Request:  opcode(4) + mr[3](12) + shmem(4096) = 4112
 *     Reply:    mr[4](16) + shmem(4096) = 4112
 *
 * INVARIANT: cc_pd contains ZERO policy.  It is a relay only.
 * No routing logic beyond opcode dispatch and dev_type field lives here.
 *
 * Relay stubs: outbound seL4_Call paths to downstream PDs are implemented
 * as well-formed CC_OK stubs (empty data) until inter-PD endpoint wiring
 * is complete in Phase 5.
 *
 * Priority: 160
 * Mode: VirtIO polled loop; seL4_Yield while used ring empty to avoid starving PDs
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/cc_contract.h"
#include "contracts/vibeos_contract.h"
#include "contracts/guest_contract.h"
#include "contracts/framebuffer_contract.h"
#include "contracts/log_drain_contract.h"
#include "contracts/agent_pool_contract.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ─── VirtIO MMIO serial driver ──────────────────────────────────────────── */
/*
 * Transport: virtio-serial-device on QEMU virtio-mmio-bus.2 (PA 0x0A000400).
 * QEMU bridges the virtserialport named "cc.0" to build/cc_pd.sock.
 *
 * The root task allocates three 4K frames and identity-maps them (VA=PA) into
 * cc_pd's VSpace, then writes the three PAs into a shared startup record page
 * at CC_PD_STARTUP_VA before starting cc_pd.  The three pages are:
 *   [0] VQ struct page — descriptor tables, avail/used rings for TX+RX queues
 *   [1] TX data buffer
 *   [2] RX data buffer
 *
 * We use a single descriptor per queue (VQ_DEPTH=4 slots, one in flight at a
 * time) and poll the used ring with seL4_Yield so other PDs can run.
 *
 * Wire frame sizes (4112 bytes) exceed the 4096-byte buffer page, so TX and RX
 * loop in ≤4096-byte chunks.  The protocol is strictly sequential (one reply
 * per request), so no RX overflow can occur across frame boundaries.
 */

#define CC_PD_VIRTIO_VA   0x10002000UL   /* VirtIO MMIO page mapped by root task */
#define CC_PD_STARTUP_VA  0x10003000UL   /* Startup record: VQ PAs from root task */
#define CC_PD_UART_DBG_VA 0x10004000UL   /* PL011 UART0 for direct debug output */
#define VMMIO_SLOT_OFF    (2u * 0x200u)  /* bus.2 → offset +0x400 within the page */

/* ─── Direct UART0 debug output (bypasses CONFIG_PRINTING) ──────────────── */

#define CC_UART_DR   (*(volatile uint32_t *)(CC_PD_UART_DBG_VA + 0x000u))
#define CC_UART_FR   (*(volatile uint32_t *)(CC_PD_UART_DBG_VA + 0x018u))
#define CC_FR_TXFF   (1u << 5)

static void cc_dbg_putc(char c)
{
    /* No TXFF spin: with seL4 MCS 10ms budget, a tight spin exhausts the
     * budget before QEMU's UART FIFO (16 bytes at 115200) gets a chance to
     * drain.  Writing unconditionally is safe on QEMU TCG; worst case one
     * character is silently dropped when the FIFO is momentarily full.   */
    CC_UART_DR = (uint32_t)(unsigned char)c;
}
static void cc_dbg_puts(const char *s)
{
    for (; *s; s++) cc_dbg_putc(*s);
}
static void cc_dbg_hex(uint64_t v)
{
    cc_dbg_puts("0x");
    for (int sh = 60; sh >= 0; sh -= 4) {
        uint32_t n = (uint32_t)((v >> (uint32_t)sh) & 0xFu);
        cc_dbg_putc(n < 10u ? (char)('0'+n) : (char)('a'+n-10u));
    }
}

/* VirtIO MMIO register offsets (relative to slot base) */
#define VMMIO_MAGIC           0x000u
#define VMMIO_VERSION         0x004u
#define VMMIO_DEVICE_ID       0x008u
#define VMMIO_DEV_FEAT        0x010u   /* DeviceFeatures (R): read current features word */
#define VMMIO_DEV_FEAT_SEL    0x014u   /* DeviceFeaturesSel (W): select features word to read */
#define VMMIO_DRV_FEAT        0x020u   /* DriverFeatures (W): write accepted features word */
#define VMMIO_DRV_FEAT_SEL    0x024u   /* DriverFeaturesSel (W): select features word to write */
#define VMMIO_QUEUE_SEL       0x030u
#define VMMIO_QUEUE_NUM_MAX   0x034u
#define VMMIO_QUEUE_NUM       0x038u
#define VMMIO_QUEUE_READY     0x044u
#define VMMIO_QUEUE_NOTIFY    0x050u
#define VMMIO_STATUS          0x070u
#define VMMIO_Q_DESC_LO       0x080u
#define VMMIO_Q_DESC_HI       0x084u
#define VMMIO_Q_AVAIL_LO      0x090u
#define VMMIO_Q_AVAIL_HI      0x094u
#define VMMIO_Q_USED_LO       0x0A0u
#define VMMIO_Q_USED_HI       0x0A4u

#define VSTATUS_ACK       1u
#define VSTATUS_DRIVER    2u
#define VSTATUS_FEAT_OK   8u
#define VSTATUS_DRIVER_OK 4u
#define VSTATUS_FAILED    128u
#define VIRTIO_MAGIC      0x74726976u
#define VIRTIO_ID_CONSOLE 3u
#define VQ_DEPTH          4u

typedef struct { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; }
    __attribute__((packed)) vq_desc_t;
typedef struct { uint16_t flags; uint16_t idx; uint16_t ring[VQ_DEPTH]; uint16_t used_event; }
    __attribute__((packed)) vq_avail_t;
typedef struct { uint32_t id; uint32_t len; } __attribute__((packed)) vq_used_elem_t;
typedef struct { uint16_t flags; uint16_t idx; vq_used_elem_t ring[VQ_DEPTH]; uint16_t avail_event; }
    __attribute__((packed)) vq_used_t;

/* Offsets within VQ struct page (all alignment requirements met) */
#define TX_DESC_OFF   0u    /* 4 × 16 B = 64 B; 16-byte aligned */
#define TX_AVAIL_OFF  128u  /* 14 B; 2-byte aligned */
#define TX_USED_OFF   256u  /* 38 B; 4-byte aligned */
#define RX_DESC_OFF   512u  /* 64 B; 16-byte aligned */
#define RX_AVAIL_OFF  640u  /* 14 B; 2-byte aligned */
#define RX_USED_OFF   768u  /* 38 B; 4-byte aligned */

static seL4_Word          g_vq_pa[3];       /* [0]=structs, [1]=TX buf, [2]=RX buf */
static volatile uint32_t *g_virtio;         /* VirtIO MMIO base at bus.2 slot */
static uint16_t           g_rx_used_last;   /* shadow of RX used ring consumer idx */

/* VQ struct page is identity-mapped: virtual address == physical address */
#define QP       ((uintptr_t)g_vq_pa[0])
#define TX_DESC  ((volatile vq_desc_t  *)(QP + TX_DESC_OFF))
#define TX_AVAIL ((volatile vq_avail_t *)(QP + TX_AVAIL_OFF))
#define TX_USED  ((volatile vq_used_t  *)(QP + TX_USED_OFF))
#define RX_DESC  ((volatile vq_desc_t  *)(QP + RX_DESC_OFF))
#define RX_AVAIL ((volatile vq_avail_t *)(QP + RX_AVAIL_OFF))
#define RX_USED  ((volatile vq_used_t  *)(QP + RX_USED_OFF))

static inline uint32_t vio_rd(uint32_t off)
{
    return *(volatile uint32_t *)((uintptr_t)g_virtio + off);
}
static inline void vio_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)((uintptr_t)g_virtio + off) = val;
}
#if defined(__aarch64__)
#define VQ_MB() __asm__ volatile("dsb sy" ::: "memory")
#elif defined(__riscv)
#define VQ_MB() __asm__ volatile("fence rw,rw" ::: "memory")
#elif defined(__x86_64__)
#define VQ_MB() __asm__ volatile("mfence" ::: "memory")
#else
#define VQ_MB() __asm__ volatile("" ::: "memory")
#endif

static void vio_queue_setup(uint32_t qidx,
                             seL4_Word desc_pa, seL4_Word avail_pa, seL4_Word used_pa)
{
    vio_wr(VMMIO_QUEUE_SEL,   qidx);
    vio_wr(VMMIO_QUEUE_NUM,   VQ_DEPTH);
    vio_wr(VMMIO_Q_DESC_LO,   (uint32_t)(desc_pa  & 0xFFFFFFFFu));
    vio_wr(VMMIO_Q_DESC_HI,   (uint32_t)(desc_pa  >> 32u));
    vio_wr(VMMIO_Q_AVAIL_LO,  (uint32_t)(avail_pa & 0xFFFFFFFFu));
    vio_wr(VMMIO_Q_AVAIL_HI,  (uint32_t)(avail_pa >> 32u));
    vio_wr(VMMIO_Q_USED_LO,   (uint32_t)(used_pa  & 0xFFFFFFFFu));
    vio_wr(VMMIO_Q_USED_HI,   (uint32_t)(used_pa  >> 32u));
    vio_wr(VMMIO_QUEUE_READY, 1u);
}

static void virtio_serial_init(void)
{
    /* VQ PAs written by root task into startup record before cc_pd started */
    volatile seL4_Word *sp = (volatile seL4_Word *)CC_PD_STARTUP_VA;
    g_vq_pa[0] = sp[0];
    g_vq_pa[1] = sp[1];
    g_vq_pa[2] = sp[2];

    g_virtio = (volatile uint32_t *)(CC_PD_VIRTIO_VA + VMMIO_SLOT_OFF);

    uint32_t magic   = vio_rd(VMMIO_MAGIC);
    uint32_t version = vio_rd(VMMIO_VERSION);
    uint32_t devid   = vio_rd(VMMIO_DEVICE_ID);
    cc_dbg_puts("[cc_pd] VirtIO magic="); cc_dbg_hex(magic);
    cc_dbg_puts(" ver="); cc_dbg_hex(version);
    cc_dbg_puts(" devid="); cc_dbg_hex(devid);
    cc_dbg_puts("\n");

    if (magic != VIRTIO_MAGIC || devid != VIRTIO_ID_CONSOLE) {
        cc_dbg_puts("[cc_pd] VirtIO init FAILED: bad magic/devid\n");
        return;
    }

    /* VirtIO 1.0 initialisation sequence */
    vio_wr(VMMIO_STATUS, 0u);
    vio_wr(VMMIO_STATUS, VSTATUS_ACK);
    vio_wr(VMMIO_STATUS, VSTATUS_ACK | VSTATUS_DRIVER);
    /* Negotiate features: read both 32-bit words, accept them with MULTIPORT
     * cleared (bit 1 of word 0) and VIRTIO_F_VERSION_1 set (bit 0 of word 1).
     * Without VIRTIO_F_VERSION_1 the device falls back to legacy mode where
     * QueueDescLow/High and QueueReady do not exist. */
    vio_wr(VMMIO_DEV_FEAT_SEL, 0u);
    uint32_t feat0 = vio_rd(VMMIO_DEV_FEAT);
    vio_wr(VMMIO_DEV_FEAT_SEL, 1u);
    uint32_t feat1 = vio_rd(VMMIO_DEV_FEAT);
    vio_wr(VMMIO_DRV_FEAT_SEL, 0u);
    vio_wr(VMMIO_DRV_FEAT, feat0 & ~(1u << 1u));  /* clear MULTIPORT */
    vio_wr(VMMIO_DRV_FEAT_SEL, 1u);
    vio_wr(VMMIO_DRV_FEAT, feat1);                /* accepts VIRTIO_F_VERSION_1 */
    vio_wr(VMMIO_STATUS, VSTATUS_ACK | VSTATUS_DRIVER | VSTATUS_FEAT_OK);
    uint32_t s_after = vio_rd(VMMIO_STATUS);
    cc_dbg_puts("[cc_pd] STATUS after FEAT_OK write="); cc_dbg_hex(s_after); cc_dbg_puts("\n");
    if (!(s_after & VSTATUS_FEAT_OK)) {
        cc_dbg_puts("[cc_pd] VirtIO FEAT_OK not set\n");
        vio_wr(VMMIO_STATUS, VSTATUS_FAILED);
        return;
    }

    vio_queue_setup(0u,
        g_vq_pa[0] + RX_DESC_OFF, g_vq_pa[0] + RX_AVAIL_OFF, g_vq_pa[0] + RX_USED_OFF);
    vio_queue_setup(1u,
        g_vq_pa[0] + TX_DESC_OFF, g_vq_pa[0] + TX_AVAIL_OFF, g_vq_pa[0] + TX_USED_OFF);

    vio_wr(VMMIO_STATUS,
           VSTATUS_ACK | VSTATUS_DRIVER | VSTATUS_FEAT_OK | VSTATUS_DRIVER_OK);

    /* Pre-post RX descriptor so the device can buffer incoming bytes */
    RX_DESC[0].addr  = (uint64_t)g_vq_pa[2];
    RX_DESC[0].len   = 4096u;
    RX_DESC[0].flags = 2u;  /* VIRTQ_DESC_F_WRITE: device writes into this buf */
    RX_DESC[0].next  = 0u;
    VQ_MB();
    RX_AVAIL->ring[0] = 0u;
    VQ_MB();
    RX_AVAIL->idx = 1u;
    VQ_MB();
    vio_wr(VMMIO_QUEUE_NOTIFY, 0u);
    g_rx_used_last = 0u;

    cc_dbg_puts("[cc_pd] VirtIO serial ready\n");
}

static void vio_serial_write(const void *buf, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0u) {
        uint32_t chunk = (n > 4096u) ? 4096u : n;
        __builtin_memcpy((void *)g_vq_pa[1], p, chunk);
        VQ_MB();
        TX_DESC[0].addr  = (uint64_t)g_vq_pa[1];
        TX_DESC[0].len   = chunk;
        TX_DESC[0].flags = 0u;
        TX_DESC[0].next  = 0u;
        VQ_MB();
        uint16_t old_used = TX_USED->idx;
        TX_AVAIL->ring[TX_AVAIL->idx & (uint16_t)(VQ_DEPTH - 1u)] = 0u;
        VQ_MB();
        TX_AVAIL->idx++;
        VQ_MB();
        cc_dbg_puts("[cc_pd] TX notify n="); cc_dbg_hex(n);
        cc_dbg_puts(" old_used="); cc_dbg_hex(old_used);
        cc_dbg_puts(" avail_idx="); cc_dbg_hex(TX_AVAIL->idx);
        cc_dbg_puts(" desc_addr="); cc_dbg_hex(TX_DESC[0].addr);
        cc_dbg_puts("\n");
        vio_wr(VMMIO_QUEUE_NOTIFY, 1u);
        uint16_t cur_used = TX_USED->idx;
        cc_dbg_puts("[cc_pd] TX post-notify used="); cc_dbg_hex(cur_used); cc_dbg_puts("\n");
        uint32_t spin = 0u;
        while (TX_USED->idx == old_used) {
            VQ_MB();
            seL4_Yield();
            spin++;
            if (spin <= 3u || (spin & 0xFFFFu) == 0u) {
                cc_dbg_puts("[cc_pd] TX yield spin="); cc_dbg_hex(spin);
                cc_dbg_puts(" used="); cc_dbg_hex(TX_USED->idx);
                cc_dbg_puts("\n");
            }
        }
        cc_dbg_puts("[cc_pd] TX done spin="); cc_dbg_hex(spin); cc_dbg_puts("\n");
        p += chunk;
        n -= chunk;
    }
}

static void vio_serial_read(void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n > 0u) {
        uint16_t cur;
        for (;;) {
            VQ_MB();
            cur = RX_USED->idx;
            if (cur != g_rx_used_last) { break; }
            seL4_Yield();
        }
        uint32_t got  = RX_USED->ring[g_rx_used_last & (uint16_t)(VQ_DEPTH - 1u)].len;
        VQ_MB();
        uint32_t take = (got > n) ? n : got;
        __builtin_memcpy(p, (const void *)g_vq_pa[2], take);
        p += take;
        n -= take;
        g_rx_used_last++;
        /* Re-post RX buffer */
        RX_DESC[0].addr  = (uint64_t)g_vq_pa[2];
        RX_DESC[0].len   = 4096u;
        RX_DESC[0].flags = 2u;
        RX_DESC[0].next  = 0u;
        VQ_MB();
        RX_AVAIL->ring[RX_AVAIL->idx & (uint16_t)(VQ_DEPTH - 1u)] = 0u;
        VQ_MB();
        RX_AVAIL->idx++;
        VQ_MB();
        vio_wr(VMMIO_QUEUE_NOTIFY, 0u);
    }
}

/* ─── Wire frame types ───────────────────────────────────────────────────── */
/*
 * These match the layout used by agentctl-ng and agentos_gui exactly.
 * The static_asserts enforce the 4112-byte invariant at compile time.
 */

#define CC_WIRE_SHMEM_SIZE  4096u

typedef struct {
    uint32_t opcode;
    uint32_t mr[3];             /* MR1, MR2, MR3 from caller */
    uint8_t  shmem[CC_WIRE_SHMEM_SIZE];
} cc_req_wire_t;

typedef struct {
    uint32_t mr[4];             /* MR0 (status), MR1, MR2, MR3 */
    uint8_t  shmem[CC_WIRE_SHMEM_SIZE];
} cc_reply_wire_t;

_Static_assert(sizeof(cc_req_wire_t)   == 4112u, "cc_req_wire_t size");
_Static_assert(sizeof(cc_reply_wire_t) == 4112u, "cc_reply_wire_t size");

/* ─── Session table ──────────────────────────────────────────────────────── */

typedef struct {
    bool     active;
    uint32_t client_badge;
    uint32_t state;
    uint32_t ticks_since_active;
    uint32_t resp_pending;
    uint32_t resp_len;
    uint8_t  resp[CC_MAX_RESP_BYTES];
} cc_session_t;

static cc_session_t g_sessions[CC_MAX_SESSIONS];

static int alloc_session(void)
{
    for (int i = 0; i < (int)CC_MAX_SESSIONS; i++) {
        if (!g_sessions[i].active) return i;
    }
    return -1;
}

static bool valid_session(uint32_t sid)
{
    return sid < CC_MAX_SESSIONS &&
           g_sessions[sid].active &&
           g_sessions[sid].state != (uint32_t)CC_SESSION_STATE_EXPIRED;
}

/* ─── Session management handlers ───────────────────────────────────────── */

static void handle_connect(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    int s = alloc_session();
    if (s < 0) {
        rep->mr[0] = CC_ERR_NO_SESSIONS;
        rep->mr[1] = 0u;
        return;
    }
    g_sessions[s].active             = true;
    g_sessions[s].client_badge       = req->mr[0]; /* badge in MR1 */
    g_sessions[s].state              = CC_SESSION_STATE_CONNECTED;
    g_sessions[s].ticks_since_active = 0u;
    g_sessions[s].resp_pending       = 0u;
    g_sessions[s].resp_len           = 0u;

    rep->mr[0] = CC_OK;
    rep->mr[1] = (uint32_t)s;
}

static void handle_disconnect(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t sid = req->mr[0]; /* session_id in MR1 */
    if (!valid_session(sid)) {
        rep->mr[0] = CC_ERR_BAD_SESSION;
        return;
    }
    g_sessions[sid].active = false;
    g_sessions[sid].resp_pending = 0u;
    g_sessions[sid].resp_len = 0u;
    rep->mr[0] = CC_OK;
}

static void handle_send(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t sid = req->mr[0]; /* session_id in MR1 */
    uint32_t len = req->mr[2]; /* command byte length in MR3 */

    if (!valid_session(sid)) {
        rep->mr[0] = CC_ERR_BAD_SESSION;
        rep->mr[1] = 0u;
        return;
    }
    if (len > CC_MAX_CMD_BYTES) {
        rep->mr[0] = CC_ERR_CMD_TOO_LARGE;
        rep->mr[1] = 0u;
        return;
    }

    /*
     * The real service-routing surface is the direct MSG_CC_* relay API below.
     * For the legacy session API, queue a deterministic empty success response
     * so callers can distinguish "accepted command" from "unknown opcode".
     */
    (void)req;
    g_sessions[sid].state = CC_SESSION_STATE_IDLE;
    g_sessions[sid].ticks_since_active = 0u;
    g_sessions[sid].resp_pending = 1u;
    g_sessions[sid].resp_len = 0u;

    rep->mr[0] = CC_OK;
    rep->mr[1] = 1u;
}

static void handle_recv(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t sid = req->mr[0]; /* session_id in MR1 */
    uint32_t max = req->mr[1]; /* max response bytes in MR2 */

    if (!valid_session(sid)) {
        rep->mr[0] = CC_ERR_BAD_SESSION;
        rep->mr[1] = 0u;
        return;
    }
    if (!g_sessions[sid].resp_pending) {
        rep->mr[0] = CC_ERR_NO_RESPONSE;
        rep->mr[1] = 0u;
        return;
    }

    uint32_t n = g_sessions[sid].resp_len;
    if (n > max) n = max;
    if (n > CC_WIRE_SHMEM_SIZE) n = CC_WIRE_SHMEM_SIZE;
    if (n > 0u) {
        __builtin_memcpy(rep->shmem, g_sessions[sid].resp, n);
    }

    g_sessions[sid].resp_pending = 0u;
    g_sessions[sid].resp_len = 0u;
    g_sessions[sid].state = CC_SESSION_STATE_IDLE;
    g_sessions[sid].ticks_since_active = 0u;

    rep->mr[0] = CC_OK;
    rep->mr[1] = n;
}

static void handle_status(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t sid = req->mr[0]; /* session_id in MR1 */
    if (!valid_session(sid)) {
        rep->mr[0] = CC_ERR_BAD_SESSION;
        rep->mr[1] = 0u;
        rep->mr[2] = 0u;
        rep->mr[3] = 0u;
        return;
    }
    rep->mr[0] = CC_OK;
    rep->mr[1] = g_sessions[sid].state;
    rep->mr[2] = g_sessions[sid].resp_pending ? 1u : 0u;
    rep->mr[3] = g_sessions[sid].ticks_since_active;
}

static void handle_list_sessions(cc_reply_wire_t *rep)
{
    cc_session_info_t *out = (cc_session_info_t *)rep->shmem;
    uint32_t count = 0u;

    for (uint32_t i = 0u; i < CC_MAX_SESSIONS; i++) {
        if (g_sessions[i].active) {
            out[count].session_id         = i;
            out[count].state              = g_sessions[i].state;
            out[count].client_badge       = g_sessions[i].client_badge;
            out[count].ticks_since_active = g_sessions[i].ticks_since_active;
            count++;
        }
    }
    rep->mr[0] = count;
}

/* ─── Relay stubs ────────────────────────────────────────────────────────── */
/*
 * Each handler below will call the corresponding downstream service PD via
 * seL4_Call once inter-PD endpoint wiring is complete (Phase 5).
 * For now they return CC_OK with empty/zero data so that external callers
 * (agentos_gui) get well-formed responses and can display an empty state.
 */

static void handle_list_guests(cc_reply_wire_t *rep)
{
    /* Phase 5: seL4_Call(vibe_engine_ep, MSG_VIBEOS_LIST) */
    rep->mr[0] = 0u; /* count = 0 */
}

static void handle_list_devices(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    (void)req;
    /* Phase 5: route by req->mr[0] (dev_type) to appropriate driver PD */
    rep->mr[0] = 0u; /* count = 0 */
}

static void handle_list_polecats(cc_reply_wire_t *rep)
{
    /* Phase 5: seL4_Call(agent_pool_ep, MSG_AGENTPOOL_STATUS) */
    rep->mr[0] = CC_OK;
    rep->mr[1] = 0u; /* total */
    rep->mr[2] = 0u; /* busy  */
    rep->mr[3] = 0u; /* idle  */
}

static void handle_guest_status(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    (void)req;
    /* Phase 5: seL4_Call(vibe_engine_ep, MSG_VIBEOS_STATUS, guest_handle) */
    rep->mr[0] = CC_ERR_BAD_HANDLE;
}

static void handle_device_status(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    (void)req;
    /* Phase 5: route by req->mr[0] (dev_type) */
    rep->mr[0] = CC_ERR_BAD_DEV_TYPE;
}

static void handle_attach_framebuffer(const cc_req_wire_t *req,
                                       cc_reply_wire_t *rep)
{
    (void)req;
    /* Phase 5: seL4_Call(framebuffer_ep, MSG_FB_FLIP, fb_handle) */
    rep->mr[0] = CC_ERR_BAD_HANDLE;
    rep->mr[1] = 0u;
}

static void handle_send_input(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    (void)req;
    /* Phase 5: seL4_Call(guest_ep, MSG_GUEST_SEND_INPUT, handle) */
    rep->mr[0] = CC_ERR_BAD_HANDLE;
}

static void handle_snapshot(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    (void)req;
    /* Phase 5: seL4_Call(vibe_engine_ep, MSG_VIBEOS_SNAPSHOT, handle) */
    rep->mr[0] = CC_ERR_RELAY_FAULT;
    rep->mr[1] = 0u;
    rep->mr[2] = 0u;
}

static void handle_restore(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    (void)req;
    /* Phase 5: seL4_Call(vibe_engine_ep, MSG_VIBEOS_RESTORE, ...) */
    rep->mr[0] = CC_ERR_RELAY_FAULT;
}

static void handle_log_stream(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    (void)req;
    /* Phase 5: seL4_Call(log_drain_ep, OP_LOG_WRITE, slot, pd_id) */
    rep->mr[0] = CC_OK;
    rep->mr[1] = 0u; /* bytes_drained */
}

/* ─── Dispatch ───────────────────────────────────────────────────────────── */

static void cc_dispatch(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    switch (req->opcode) {
    /* Session management */
    case MSG_CC_CONNECT:    handle_connect(req, rep);          break;
    case MSG_CC_DISCONNECT: handle_disconnect(req, rep);       break;
    case MSG_CC_SEND:       handle_send(req, rep);             break;
    case MSG_CC_RECV:       handle_recv(req, rep);             break;
    case MSG_CC_STATUS:     handle_status(req, rep);           break;
    case MSG_CC_LIST:       handle_list_sessions(rep);         break;

    /* Relay API */
    case MSG_CC_LIST_GUESTS:        handle_list_guests(rep);             break;
    case MSG_CC_LIST_DEVICES:       handle_list_devices(req, rep);       break;
    case MSG_CC_LIST_POLECATS:      handle_list_polecats(rep);           break;
    case MSG_CC_GUEST_STATUS:       handle_guest_status(req, rep);       break;
    case MSG_CC_DEVICE_STATUS:      handle_device_status(req, rep);      break;
    case MSG_CC_ATTACH_FRAMEBUFFER: handle_attach_framebuffer(req, rep); break;
    case MSG_CC_SEND_INPUT:         handle_send_input(req, rep);         break;
    case MSG_CC_SNAPSHOT:           handle_snapshot(req, rep);           break;
    case MSG_CC_RESTORE:            handle_restore(req, rep);            break;
    case MSG_CC_LOG_STREAM:         handle_log_stream(req, rep);         break;

    default:
        sel4_dbg_puts("[cc_pd] unknown opcode\n");
        rep->mr[0] = CC_ERR_BAD_SESSION;
        break;
    }
}

/* ─── Entry point ────────────────────────────────────────────────────────── */

void cc_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)my_ep;
    (void)ns_ep;

    /* Canary: unconditional DR write — appears iff cc_pd runs + UART mapped */
    *(volatile uint32_t *)CC_PD_UART_DBG_VA = (uint32_t)'[';
    *(volatile uint32_t *)CC_PD_UART_DBG_VA = (uint32_t)'@';
    *(volatile uint32_t *)CC_PD_UART_DBG_VA = (uint32_t)']';
    *(volatile uint32_t *)CC_PD_UART_DBG_VA = (uint32_t)'\n';

    /* Initialise session table */
    for (uint32_t i = 0u; i < CC_MAX_SESSIONS; i++) {
        g_sessions[i].active = false;
        g_sessions[i].state  = CC_SESSION_STATE_IDLE;
        g_sessions[i].resp_pending = 0u;
        g_sessions[i].resp_len = 0u;
    }

    /* Initialise VirtIO MMIO serial (mapped by root task; VQ PAs in startup record) */
    virtio_serial_init();
    {
        uint8_t probe = '!';
        vio_serial_write(&probe, 1u);
    }

    /* Static buffers live in BSS — kept off the stack since each frame is
     * 4112 bytes, which would exhaust cc_pd's 16 KB stack otherwise.    */
    static cc_req_wire_t   g_req;
    static cc_reply_wire_t g_rep;

    while (1) {
        vio_serial_read(&g_req, sizeof(g_req));
        __builtin_memset(&g_rep, 0, sizeof(g_rep));
        cc_dispatch(&g_req, &g_rep);
        vio_serial_write(&g_rep, sizeof(g_rep));
    }
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { cc_pd_main(my_ep, ns_ep); }
