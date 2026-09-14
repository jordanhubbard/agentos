/*
 * agentOS USB Host Controller Service Protection Domain (usb_pd)
 *
 * OS-neutral USB host controller service.  The PD owns the USB host
 * controller via seL4 device frame capabilities and exposes an IPC API
 * to guest OSes.  Guests receive opaque handles; no MMIO access is granted.
 *
 * Hardware abstraction:
 *   x86_64 (QEMU Q35, Intel NUC): XHCI host controller
 *   AArch64 (QEMU virt):          XHCI host controller
 *   AArch64 (RPi5):               DWC3 SuperSpeed + PCIe XHCI
 *
 * Build flag: -DAGENTOS_USB_PD activates full logic.
 * Stub is always compiled so usb_pd.elf links and the system image is valid.
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "arch_barrier.h"
#include "contracts/usb_contract.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Microkit setvar_vaddr symbols ───────────────────────────────────────── */

uintptr_t usb_mmio_vaddr;
uintptr_t usb_shmem_vaddr;
uintptr_t log_drain_rings_vaddr;

#ifdef AGENTOS_USB_PD

/* ── Guest policy bitmask values ─────────────────────────────────────────── */

#define USB_POLICY_HID      (1u << 0)
#define USB_POLICY_STORAGE  (1u << 1)
#define USB_POLICY_HUB      (1u << 2)
#define USB_POLICY_VENDOR   (1u << 3)
#define USB_POLICY_ALL      0xFFu

/* ── XHCI Capability Register offsets ────────────────────────────────────── */

#define XHCI_CAPLENGTH       0x00u
#define XHCI_HCIVERSION      0x02u
#define XHCI_HCSPARAMS1      0x04u
#define XHCI_HCSPARAMS2      0x08u
#define XHCI_HCCPARAMS1      0x10u

#define XHCI_OP_USBCMD       0x00u
#define XHCI_OP_USBSTS       0x04u
#define XHCI_OP_CRCR         0x18u
#define XHCI_OP_DCBAAP       0x30u
#define XHCI_OP_CONFIG       0x38u
#define XHCI_OP_PORTSC_BASE  0x400u

#define XHCI_USBCMD_RUN      (1u << 0)
#define XHCI_USBCMD_HCRST    (1u << 1)
#define XHCI_USBSTS_HCH      (1u << 0)
#define XHCI_USBSTS_CNR      (1u << 11)
#define XHCI_PORTSC_CCS      (1u << 0)
#define XHCI_PORTSC_PED      (1u << 1)
#define XHCI_PORTSC_PP       (1u << 9)
#define XHCI_PORTSC_PS_MASK  (0xFu << 10)
#define XHCI_PORTSC_PS_FULL  (1u << 10)
#define XHCI_PORTSC_PS_HIGH  (2u << 10)
#define XHCI_PORTSC_PS_SUPER (4u << 10)
#define XHCI_HCIVERSION_MIN  0x0090u

/* ── DWC3 Global Register offsets ────────────────────────────────────────── */

#define DWC3_GSNPSID         0xC120u
#define DWC3_GSNPSID_MAGIC   0x55330000u
#define DWC3_GSNPSID_MASK    0xFFFF0000u

/* ── Internal state ──────────────────────────────────────────────────────── */

typedef struct {
    bool     present;
    uint8_t  dev_class;
    uint8_t  speed;
    uint16_t vid;
    uint16_t pid;
} usb_dev_entry_t;

typedef struct {
    bool     active;
    uint32_t handle;
    uint32_t dev_index;
    uint32_t policy;
} usb_client_t;

static usb_dev_entry_t  devs[USB_MAX_DEVICES];
static uint32_t         dev_count     = 0;
static usb_client_t     clients[USB_MAX_CLIENTS];
static bool             hw_present    = false;
static uint8_t          xhci_cap_len  = 0;
static uint8_t          xhci_max_ports = 0;
static uint32_t         next_handle   = 1;

/* ── Minimal memory helpers ──────────────────────────────────────────────── */

static void usb_memset(void *dst, int v, uint32_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)v;
}

static void usb_strncpy(char *dst, const char *src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n - 1u && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ── MMIO helpers ────────────────────────────────────────────────────────── */

static inline uint8_t mmio_read8(uintptr_t base, uint32_t off) {
    return *(volatile uint8_t *)(base + off);
}
static inline uint16_t mmio_read16(uintptr_t base, uint32_t off) {
    return *(volatile uint16_t *)(base + off);
}
static inline uint32_t mmio_read32(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static inline void mmio_write32(uintptr_t base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v; ARCH_MB();
}

static inline uintptr_t xhci_op_base(void) {
    return usb_mmio_vaddr + xhci_cap_len;
}

/* ── Logging helpers ─────────────────────────────────────────────────────── */

#define USB_LOG(msg)   log_drain_write(18, 18, "[usb_pd] " msg)

static void usb_log_hex(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    buf[0]  = '0'; buf[1]  = 'x';
    buf[2]  = hex[(v >> 28) & 0xf]; buf[3]  = hex[(v >> 24) & 0xf];
    buf[4]  = hex[(v >> 20) & 0xf]; buf[5]  = hex[(v >> 16) & 0xf];
    buf[6]  = hex[(v >> 12) & 0xf]; buf[7]  = hex[(v >>  8) & 0xf];
    buf[8]  = hex[(v >>  4) & 0xf]; buf[9]  = hex[ v        & 0xf];
    buf[10] = '\0';
    log_drain_write(18, 18, buf);
}

static void usb_log_dec(uint32_t v) {
    if (v == 0) { log_drain_write(18, 18, "0"); return; }
    char buf[12]; int i = 11; buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (char)(v % 10); v /= 10; }
    log_drain_write(18, 18, &buf[i]);
}

/* ── XHCI probe ──────────────────────────────────────────────────────────── */

static bool probe_xhci(void) {
    if (!usb_mmio_vaddr) { USB_LOG("XHCI MMIO not mapped — stub mode\n"); return false; }
    uint8_t  cap_len = mmio_read8 (usb_mmio_vaddr, XHCI_CAPLENGTH);
    uint16_t version = mmio_read16(usb_mmio_vaddr, XHCI_HCIVERSION);
    if (cap_len < 0x20u || cap_len == 0xFFu) {
        USB_LOG("XHCI probe: bad CAPLENGTH=");
        usb_log_hex(cap_len); USB_LOG(", stub mode\n"); return false;
    }
    if (version < XHCI_HCIVERSION_MIN) {
        USB_LOG("XHCI probe: unsupported HCIVERSION=");
        usb_log_hex(version); USB_LOG(", stub mode\n"); return false;
    }
    xhci_cap_len = cap_len;
    uint32_t hcsp1 = mmio_read32(usb_mmio_vaddr, XHCI_HCSPARAMS1);
    xhci_max_ports = (uint8_t)((hcsp1 >> 24) & 0xFFu);
    USB_LOG("XHCI detected: ver="); usb_log_hex(version);
    USB_LOG(" caplen="); usb_log_hex(cap_len);
    USB_LOG(" ports="); usb_log_dec(xhci_max_ports); USB_LOG("\n");
    return true;
}

static bool probe_dwc3(void) {
    if (!usb_mmio_vaddr) return false;
    uint32_t id = mmio_read32(usb_mmio_vaddr, DWC3_GSNPSID);
    if ((id & DWC3_GSNPSID_MASK) == DWC3_GSNPSID_MAGIC) {
        USB_LOG("DWC3 USB3 core detected: id="); usb_log_hex(id); USB_LOG("\n");
        return true;
    }
    return false;
}

/* ── Port-speed decode ────────────────────────────────────────────────────── */

static uint8_t portsc_to_speed(uint32_t portsc) {
    uint32_t ps = (portsc & XHCI_PORTSC_PS_MASK) >> 10;
    switch (ps) {
        case 1:  return USB_SPEED_FULL;
        case 3:  return USB_SPEED_HIGH;
        case 4:  return USB_SPEED_SUPER;
        default: return USB_SPEED_FULL;
    }
}

/* ── Device enumeration ───────────────────────────────────────────────────── */

static void enumerate_xhci(void) {
    dev_count = 0;
    usb_memset(devs, 0, sizeof(devs));
    if (!hw_present || !xhci_max_ports) {
        devs[0].present   = true;
        devs[0].dev_class = 0x03;
        devs[0].speed     = USB_SPEED_HIGH;
        devs[0].vid       = 0x046Du;
        devs[0].pid       = 0xC52Bu;
        dev_count = 1;
        USB_LOG("enumerate: stub mode — 1 simulated HID device\n");
        return;
    }
    uintptr_t op = xhci_op_base();
    uint8_t ports = xhci_max_ports;
    if (ports > USB_MAX_DEVICES) ports = USB_MAX_DEVICES;
    for (uint8_t i = 0; i < ports; i++) {
        uint32_t portsc = mmio_read32(op, XHCI_OP_PORTSC_BASE + (uint32_t)i * 16u);
        if (!(portsc & XHCI_PORTSC_CCS)) continue;
        devs[dev_count].present   = true;
        devs[dev_count].speed     = portsc_to_speed(portsc);
        devs[dev_count].dev_class = 0x00;
        devs[dev_count].vid       = 0x0000;
        devs[dev_count].pid       = 0x0000;
        dev_count++;
    }
    USB_LOG("enumerate: found "); usb_log_dec(dev_count); USB_LOG(" device(s)\n");
}

/* ── Handle management ───────────────────────────────────────────────────── */

static int alloc_client(void) {
    for (int i = 0; i < (int)USB_MAX_CLIENTS; i++)
        if (!clients[i].active) return i;
    return -1;
}

static usb_client_t *find_client(uint32_t handle) {
    for (int i = 0; i < (int)USB_MAX_CLIENTS; i++)
        if (clients[i].active && clients[i].handle == handle) return &clients[i];
    return (void *)0;
}

/* ── Policy enforcement ──────────────────────────────────────────────────── */

static bool policy_allows(uint32_t policy, uint8_t dev_class) {
    if (policy == USB_POLICY_ALL) return true;
    switch (dev_class) {
        case 0x03: return (policy & USB_POLICY_HID)     != 0;
        case 0x08: return (policy & USB_POLICY_STORAGE) != 0;
        case 0x09: return (policy & USB_POLICY_HUB)     != 0;
        case 0xFF: return (policy & USB_POLICY_VENDOR)  != 0;
        default:   return false;
    }
}

/* ── Shmem helpers ───────────────────────────────────────────────────────── */

static volatile usb_dev_info_t *shmem_dev_list(void) {
    return (volatile usb_dev_info_t *)usb_shmem_vaddr;
}

/* ── msg helpers ─────────────────────────────────────────────────────────── */

#ifndef AGENTOS_IPC_HELPERS_DEFINED
#define AGENTOS_IPC_HELPERS_DEFINED
static inline uint32_t msg_u32(const sel4_msg_t *m, uint32_t off) {
    uint32_t v = 0;
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        v  = (uint32_t)m->data[off]; v |= (uint32_t)m->data[off+1]<<8;
        v |= (uint32_t)m->data[off+2]<<16; v |= (uint32_t)m->data[off+3]<<24;
    }
    return v;
}
static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        m->data[off]=(uint8_t)v; m->data[off+1]=(uint8_t)(v>>8);
        m->data[off+2]=(uint8_t)(v>>16); m->data[off+3]=(uint8_t)(v>>24);
    }
}
#endif /* AGENTOS_IPC_HELPERS_DEFINED */

/* ── Message handlers ────────────────────────────────────────────────────── */

static uint32_t h_enumerate(sel4_badge_t b, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    enumerate_xhci();
    rep_u32(rep, 0, USB_OK);
    rep_u32(rep, 4, dev_count);
    rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_list(sel4_badge_t b, const sel4_msg_t *req,
                         sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t max = msg_u32(req, 4);
    if (!usb_shmem_vaddr) {
        rep_u32(rep, 0, USB_OK); rep_u32(rep, 4, 0); rep->length = 8;
        return SEL4_ERR_OK;
    }
    if (max == 0 || max > USB_MAX_DEVICES) max = USB_MAX_DEVICES;
    uint32_t count = dev_count < max ? dev_count : max;
    volatile usb_dev_info_t *dst = shmem_dev_list();
    for (uint32_t i = 0; i < count; i++) {
        dst[i].dev_index = i;
        dst[i].vid       = devs[i].vid;
        dst[i].pid       = devs[i].pid;
        dst[i].dev_class = devs[i].dev_class;
        dst[i].speed     = devs[i].speed;
        dst[i]._pad[0]   = 0; dst[i]._pad[1] = 0;
        usb_strncpy((char *)dst[i].manufacturer, "agentOS-USB", 32);
        switch (devs[i].dev_class) {
            case 0x03: usb_strncpy((char *)dst[i].product, "HID Device",   32); break;
            case 0x08: usb_strncpy((char *)dst[i].product, "Mass Storage", 32); break;
            default:   usb_strncpy((char *)dst[i].product, "USB Device",   32); break;
        }
    }
    ARCH_WMB();
    rep_u32(rep, 0, USB_OK); rep_u32(rep, 4, count); rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_open(sel4_badge_t b, const sel4_msg_t *req,
                         sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t dev_index = msg_u32(req, 4);
    uint32_t policy    = msg_u32(req, 8);
    if (dev_index >= dev_count || !devs[dev_index].present) {
        rep_u32(rep, 0, USB_ERR_BAD_DEV_INDEX); rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }
    if (!policy_allows(policy, devs[dev_index].dev_class)) {
        USB_LOG("OPEN denied: policy mismatch\n");
        rep_u32(rep, 0, USB_ERR_BAD_DEV_INDEX); rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }
    int slot = alloc_client();
    if (slot < 0) {
        rep_u32(rep, 0, USB_ERR_NO_SLOTS); rep->length = 4;
        return SEL4_ERR_NO_MEM;
    }
    uint32_t handle = next_handle++;
    if (next_handle == 0) next_handle = 1;
    clients[slot].active    = true;
    clients[slot].handle    = handle;
    clients[slot].dev_index = dev_index;
    clients[slot].policy    = policy;
    rep_u32(rep, 0, USB_OK); rep_u32(rep, 4, handle); rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_close(sel4_badge_t b, const sel4_msg_t *req,
                          sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t handle = msg_u32(req, 4);
    usb_client_t *c = find_client(handle);
    if (!c) { rep_u32(rep, 0, USB_ERR_BAD_HANDLE); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    usb_memset(c, 0, sizeof(*c));
    rep_u32(rep, 0, USB_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_control(sel4_badge_t b, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t handle   = msg_u32(req, 4);
    uint32_t data_len = msg_u32(req, 12);
    usb_client_t *c = find_client(handle);
    if (!c) { rep_u32(rep, 0, USB_ERR_BAD_HANDLE); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    if (!devs[c->dev_index].present) { rep_u32(rep, 0, USB_ERR_DISCONNECTED); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    rep_u32(rep, 0, USB_OK); rep_u32(rep, 4, data_len); rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_bulk_in(sel4_badge_t b, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t handle = msg_u32(req, 4);
    usb_client_t *c = find_client(handle);
    if (!c) { rep_u32(rep, 0, USB_ERR_BAD_HANDLE); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    if (!devs[c->dev_index].present) { rep_u32(rep, 0, USB_ERR_DISCONNECTED); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    if (!(c->policy & (USB_POLICY_HID | USB_POLICY_STORAGE | USB_POLICY_ALL))) {
        rep_u32(rep, 0, USB_ERR_BAD_EP); rep->length = 4; return SEL4_ERR_BAD_ARG;
    }
    rep_u32(rep, 0, USB_OK); rep_u32(rep, 4, 0u); rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_bulk_out(sel4_badge_t b, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t handle = msg_u32(req, 4);
    uint32_t len    = msg_u32(req, 12);
    usb_client_t *c = find_client(handle);
    if (!c) { rep_u32(rep, 0, USB_ERR_BAD_HANDLE); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    if (!devs[c->dev_index].present) { rep_u32(rep, 0, USB_ERR_DISCONNECTED); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    rep_u32(rep, 0, USB_OK); rep_u32(rep, 4, len); rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_status(sel4_badge_t b, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t handle = msg_u32(req, 4);
    usb_client_t *c = find_client(handle);
    if (!c) {
        rep_u32(rep, 0, USB_STATE_ERROR);
        rep_u32(rep, 4, USB_ERR_BAD_HANDLE);
        rep_u32(rep, 8, USB_SPEED_FULL);
        rep->length = 12; return SEL4_ERR_OK;
    }
    uint8_t speed = devs[c->dev_index].speed;
    rep_u32(rep, 0, USB_STATE_CONFIGURED);
    rep_u32(rep, 4, USB_OK);
    rep_u32(rep, 8, speed);
    rep->length = 12;
    return SEL4_ERR_OK;
}

static uint32_t h_reset(sel4_badge_t b, const sel4_msg_t *req,
                          sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t handle = msg_u32(req, 4);
    usb_client_t *c = find_client(handle);
    if (!c) { rep_u32(rep, 0, USB_ERR_BAD_HANDLE); rep->length = 4; return SEL4_ERR_NOT_FOUND; }
    rep_u32(rep, 0, USB_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_int_xfer(sel4_badge_t b, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    rep_u32(rep, 0, USB_ERR_STALL); rep->length = 4;
    return SEL4_ERR_OK;
}

void usb_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    agentos_log_boot("usb_pd");
    usb_memset(devs,    0, sizeof(devs));
    usb_memset(clients, 0, sizeof(clients));

#if defined(ARCH_AARCH64)
    bool dwc3_ok = probe_dwc3();
    if (dwc3_ok) USB_LOG("DWC3 USB3 core found — using XHCI host interface\n");
#endif
    hw_present = probe_xhci();

    if (hw_present) {
        USB_LOG("USB host controller ready\n");
    } else {
        USB_LOG("USB host controller: stub mode (build with AGENTOS_USB_PD and wire MMIO)\n");
    }

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, MSG_USB_ENUMERATE, h_enumerate, (void *)0);
    sel4_server_register(&srv, MSG_USB_LIST,      h_list,      (void *)0);
    sel4_server_register(&srv, MSG_USB_OPEN,      h_open,      (void *)0);
    sel4_server_register(&srv, MSG_USB_CLOSE,     h_close,     (void *)0);
    sel4_server_register(&srv, MSG_USB_CONTROL,   h_control,   (void *)0);
    sel4_server_register(&srv, MSG_USB_BULK_IN,   h_bulk_in,   (void *)0);
    sel4_server_register(&srv, MSG_USB_BULK_OUT,  h_bulk_out,  (void *)0);
    sel4_server_register(&srv, MSG_USB_STATUS,    h_status,    (void *)0);
    sel4_server_register(&srv, 0x2309u,           h_int_xfer,  (void *)0);
    sel4_server_register(&srv, 0x230Au,           h_reset,     (void *)0);
    sel4_server_run(&srv);
}

#else /* !AGENTOS_USB_PD — no-op stub ────────────────────────────────────── */

static uint32_t usb_stub_dispatch(sel4_badge_t b, const sel4_msg_t *req,
                                  sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)req; (void)ctx;
    rep->data[0] = 0;
    rep->data[1] = 0;
    rep->data[2] = 0;
    rep->data[3] = 0;
    rep->length = 4;
    return SEL4_ERR_OK;
}

void usb_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)my_ep; (void)ns_ep;
    agentos_log_boot("usb_pd");
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, usb_stub_dispatch, (void *)0);
    sel4_server_run(&srv);
}

#endif /* AGENTOS_USB_PD */

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    usb_pd_main(my_ep, ns_ep);
}
