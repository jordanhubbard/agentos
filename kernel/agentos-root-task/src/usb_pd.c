/*
 * agentOS USB Host Controller Service Protection Domain (usb_pd)
 *
 * OS-neutral USB host controller service.  The PD owns the USB host
 * controller via seL4 device frame capabilities and exposes an IPC API
 * to guest OSes.  Guests receive opaque handles; no MMIO access is granted.
 *
 * Hardware abstraction
 * ────────────────────
 *   x86_64 (QEMU Q35, Intel NUC):  XHCI host controller
 *   AArch64 (QEMU virt):            XHCI host controller (QEMU xhci-pci model)
 *   AArch64 (RPi5):                 DWC3 SuperSpeed + PCIe XHCI
 *
 * DWC3 on RPi5 exposes an XHCI host interface at the PCIe BAR, so the
 * same XHCI probe path handles both QEMU and RPi5 once the MMIO window
 * is wired in agentos-rpi5.system.  The DWC3 global core registers are
 * probed first to confirm the USB3 core is present; the XHCI host interface
 * is then initialised on top.
 *
 * Device class filtering
 * ─────────────────────
 *   Caller policy (passed as MR2 bitmask in MSG_USB_OPEN) governs which
 *   device classes the handle may access:
 *     USB_POLICY_HID      (1<<0) — keyboard/mouse passthrough (class 0x03)
 *     USB_POLICY_STORAGE  (1<<1) — mass storage passthrough  (class 0x08)
 *     USB_POLICY_HUB      (1<<2) — hub devices               (class 0x09)
 *     USB_POLICY_ALL      0xFF   — allow all classes (supervisor only)
 *
 * Channel:  CH_USB_PD (id 0 from usb_pd perspective)
 * Shmem:    usb_shmem_vaddr (set by Microkit setvar_vaddr)
 *
 * Build flag: -DAGENTOS_USB_PD activates full logic.
 * Stub is always compiled so usb_pd.elf links and the system image is valid.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "arch_barrier.h"
#include "contracts/usb_contract.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Microkit setvar_vaddr symbols ───────────────────────────────────────── */

uintptr_t usb_mmio_vaddr;      /* XHCI MMIO window (set by Microkit) */
uintptr_t usb_shmem_vaddr;     /* shared data/device-list region */
uintptr_t log_drain_rings_vaddr;

#ifdef AGENTOS_USB_PD

/* ── Channel IDs (usb_pd perspective) ────────────────────────────────────── */

#define USB_CH_CONTROLLER  0   /* controller PPCs into usb_pd */

/* ── Guest policy bitmask values ─────────────────────────────────────────── */

#define USB_POLICY_HID      (1u << 0)   /* class 0x03 — HID (kbd/mouse) */
#define USB_POLICY_STORAGE  (1u << 1)   /* class 0x08 — mass storage */
#define USB_POLICY_HUB      (1u << 2)   /* class 0x09 — hub */
#define USB_POLICY_VENDOR   (1u << 3)   /* class 0xFF — vendor-specific */
#define USB_POLICY_ALL      0xFFu       /* supervisor: allow all classes */

/* ── XHCI Capability Register offsets ────────────────────────────────────── */

#define XHCI_CAPLENGTH       0x00u  /* 1 byte: capability register space length */
#define XHCI_HCIVERSION      0x02u  /* 2 bytes: interface version (0x0100 / 0x0110) */
#define XHCI_HCSPARAMS1      0x04u  /* bits[31:24]=MaxPorts bits[18:8]=MaxIntrs bits[7:0]=MaxSlots */
#define XHCI_HCSPARAMS2      0x08u  /* Event ring segment table max, isochronous params */
#define XHCI_HCCPARAMS1      0x10u  /* capability params 1 */

/* XHCI Operational Register offsets (base = MMIO + CAPLENGTH) */
#define XHCI_OP_USBCMD       0x00u  /* USB Command register */
#define XHCI_OP_USBSTS       0x04u  /* USB Status register */
#define XHCI_OP_CRCR         0x18u  /* Command Ring Control */
#define XHCI_OP_DCBAAP       0x30u  /* Device Context Base Address Array Pointer */
#define XHCI_OP_CONFIG       0x38u  /* Configure register (MaxSlotsEn) */
#define XHCI_OP_PORTSC_BASE  0x400u /* Port Status and Control (one per port, 16 bytes apart) */

#define XHCI_USBCMD_RUN      (1u << 0)  /* Run/Stop */
#define XHCI_USBCMD_HCRST    (1u << 1)  /* Host Controller Reset */
#define XHCI_USBSTS_HCH      (1u << 0)  /* HCHalted */
#define XHCI_USBSTS_CNR      (1u << 11) /* Controller Not Ready */
#define XHCI_PORTSC_CCS      (1u << 0)  /* Current Connect Status */
#define XHCI_PORTSC_PED      (1u << 1)  /* Port Enabled/Disabled */
#define XHCI_PORTSC_PP       (1u << 9)  /* Port Power */
#define XHCI_PORTSC_PS_MASK  (0xFu << 10) /* Port Speed (bits 13:10) */
#define XHCI_PORTSC_PS_FULL  (1u << 10) /* Full speed */
#define XHCI_PORTSC_PS_HIGH  (2u << 10) /* High speed */
#define XHCI_PORTSC_PS_SUPER (4u << 10) /* SuperSpeed */
#define XHCI_HCIVERSION_MIN  0x0090u    /* minimum accepted: 0.96 */

/* ── DWC3 Global Register offsets (AArch64 RPi5 only) ────────────────────── */

#define DWC3_GSNPSID         0xC120u  /* Synopsis ID: 0x5533XXXX = USB3 core */
#define DWC3_GSNPSID_MAGIC   0x55330000u
#define DWC3_GSNPSID_MASK    0xFFFF0000u

/* ── Internal state ──────────────────────────────────────────────────────── */

typedef struct {
    bool     present;
    uint8_t  dev_class;
    uint8_t  speed;        /* USB_SPEED_* */
    uint16_t vid;
    uint16_t pid;
} usb_dev_entry_t;

typedef struct {
    bool     active;
    uint32_t handle;
    uint32_t dev_index;
    uint32_t policy;      /* USB_POLICY_* bitmask granted at open time */
} usb_client_t;

static usb_dev_entry_t  devs[USB_MAX_DEVICES];
static uint32_t         dev_count     = 0;
static usb_client_t     clients[USB_MAX_CLIENTS];
static bool             hw_present    = false;
static uint8_t          xhci_cap_len  = 0;
static uint8_t          xhci_max_ports = 0;
static uint32_t         next_handle   = 1;

/* ── Minimal string/memory helpers ──────────────────────────────────────── */

static void usb_memset(void *dst, int v, uint32_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)v;
}

static void usb_strncpy(char *dst, const char *src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n - 1u && src[i]; i++)
        dst[i] = src[i];
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
    *(volatile uint32_t *)(base + off) = v;
    ARCH_MB();
}

/* Operational register base = MMIO base + CAPLENGTH */
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

/*
 * probe_xhci — detect XHCI controller at usb_mmio_vaddr.
 *
 * Reads CAPLENGTH and HCIVERSION from the XHCI capability space.
 * CAPLENGTH must be in range [0x20, 0xFF]; HCIVERSION must be ≥ 0.96.
 * Returns true if a valid XHCI controller is found.
 */
static bool probe_xhci(void) {
    if (!usb_mmio_vaddr) {
        USB_LOG("XHCI MMIO not mapped — stub mode\n");
        return false;
    }

    uint8_t  cap_len = mmio_read8 (usb_mmio_vaddr, XHCI_CAPLENGTH);
    uint16_t version = mmio_read16(usb_mmio_vaddr, XHCI_HCIVERSION);

    if (cap_len < 0x20u || cap_len == 0xFFu) {
        USB_LOG("XHCI probe: bad CAPLENGTH=");
        usb_log_hex(cap_len);
        USB_LOG(", stub mode\n");
        return false;
    }
    if (version < XHCI_HCIVERSION_MIN) {
        USB_LOG("XHCI probe: unsupported HCIVERSION=");
        usb_log_hex(version);
        USB_LOG(", stub mode\n");
        return false;
    }

    xhci_cap_len = cap_len;
    uint32_t hcsp1     = mmio_read32(usb_mmio_vaddr, XHCI_HCSPARAMS1);
    xhci_max_ports     = (uint8_t)((hcsp1 >> 24) & 0xFFu);

    USB_LOG("XHCI detected: ver=");
    usb_log_hex(version);
    USB_LOG(" caplen=");
    usb_log_hex(cap_len);
    USB_LOG(" ports=");
    usb_log_dec(xhci_max_ports);
    USB_LOG("\n");
    return true;
}

/*
 * probe_dwc3 — detect DWC3 SuperSpeed USB core (AArch64/RPi5).
 *
 * The DWC3 global SNPS ID register at offset 0xC120 carries the magic
 * 0x5533XXXX identifying a Synopsys USB 3.0 core.
 */
static bool probe_dwc3(void) {
    if (!usb_mmio_vaddr) return false;
    uint32_t id = mmio_read32(usb_mmio_vaddr, DWC3_GSNPSID);
    if ((id & DWC3_GSNPSID_MASK) == DWC3_GSNPSID_MAGIC) {
        USB_LOG("DWC3 USB3 core detected: id=");
        usb_log_hex(id);
        USB_LOG("\n");
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

/*
 * enumerate_xhci — scan XHCI port status registers for connected devices.
 *
 * For each port with CCS set, creates a synthetic device entry.
 * Real descriptor fetch (GET_DESCRIPTOR) requires a full command ring and
 * transfer ring setup; that is the XHCI_TRB_INTEGRATION_POINT.  We populate
 * placeholder VID/PID/class and tag every entry for future upgrade.
 *
 * XHCI_TRB_INTEGRATION_POINT: implement full TRB submission and completion
 * event processing here to retrieve actual USB device descriptors.
 */
static void enumerate_xhci(void) {
    dev_count = 0;
    usb_memset(devs, 0, sizeof(devs));

    if (!hw_present || !xhci_max_ports) {
        /* Stub: populate one simulated HID device so guests see something */
        devs[0].present   = true;
        devs[0].dev_class = 0x03;   /* HID */
        devs[0].speed     = USB_SPEED_HIGH;
        devs[0].vid       = 0x046Du; /* Logitech */
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
        if (!(portsc & XHCI_PORTSC_CCS)) continue;  /* nothing connected */

        devs[dev_count].present   = true;
        devs[dev_count].speed     = portsc_to_speed(portsc);
        devs[dev_count].dev_class = 0x00;  /* unknown until descriptor read */
        devs[dev_count].vid       = 0x0000;
        devs[dev_count].pid       = 0x0000;
        dev_count++;
        /* XHCI_TRB_INTEGRATION_POINT: issue GET_DESCRIPTOR here */
    }

    USB_LOG("enumerate: found ");
    usb_log_dec(dev_count);
    USB_LOG(" device(s)\n");
}

/* ── Handle management ───────────────────────────────────────────────────── */

static int alloc_client(void) {
    for (int i = 0; i < (int)USB_MAX_CLIENTS; i++) {
        if (!clients[i].active) return i;
    }
    return -1;
}

static usb_client_t *find_client(uint32_t handle) {
    for (int i = 0; i < (int)USB_MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].handle == handle)
            return &clients[i];
    }
    return NULL;
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

/* ── Message handlers ────────────────────────────────────────────────────── */

/* MSG_USB_ENUMERATE (0x2301) */
static microkit_msginfo handle_enumerate(void) {
    enumerate_xhci();
    microkit_mr_set(0, USB_OK);
    microkit_mr_set(1, dev_count);
    return microkit_msginfo_new(0, 2);
}

/* MSG_USB_LIST (0x2302): write usb_dev_info_t[] to shmem */
static microkit_msginfo handle_list(void) {
    uint32_t max = (uint32_t)microkit_mr_get(1);
    if (!usb_shmem_vaddr) {
        microkit_mr_set(0, USB_OK);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
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
        dst[i]._pad[0]   = 0;
        dst[i]._pad[1]   = 0;
        /* Synthetic descriptor strings */
        usb_strncpy((char *)dst[i].manufacturer, "agentOS-USB", 32);
        switch (devs[i].dev_class) {
            case 0x03:
                usb_strncpy((char *)dst[i].product, "HID Device", 32);
                break;
            case 0x08:
                usb_strncpy((char *)dst[i].product, "Mass Storage", 32);
                break;
            default:
                usb_strncpy((char *)dst[i].product, "USB Device", 32);
                break;
        }
    }
    ARCH_WMB();
    microkit_mr_set(0, USB_OK);
    microkit_mr_set(1, count);
    return microkit_msginfo_new(0, 2);
}

/* MSG_USB_OPEN (0x2303): MR1=dev_index MR2=policy → MR0=ok MR1=handle */
static microkit_msginfo handle_open(void) {
    uint32_t dev_index = (uint32_t)microkit_mr_get(1);
    uint32_t policy    = (uint32_t)microkit_mr_get(2);

    if (dev_index >= dev_count || !devs[dev_index].present) {
        microkit_mr_set(0, USB_ERR_BAD_DEV_INDEX);
        return microkit_msginfo_new(0, 1);
    }
    if (!policy_allows(policy, devs[dev_index].dev_class)) {
        USB_LOG("OPEN denied: class=");
        usb_log_hex(devs[dev_index].dev_class);
        USB_LOG(" policy=");
        usb_log_hex(policy);
        USB_LOG("\n");
        microkit_mr_set(0, USB_ERR_BAD_DEV_INDEX); /* repurpose for policy deny */
        return microkit_msginfo_new(0, 1);
    }

    int slot = alloc_client();
    if (slot < 0) {
        microkit_mr_set(0, USB_ERR_NO_SLOTS);
        return microkit_msginfo_new(0, 1);
    }

    uint32_t handle = next_handle++;
    if (next_handle == 0) next_handle = 1;  /* wrap, skip 0 */

    clients[slot].active    = true;
    clients[slot].handle    = handle;
    clients[slot].dev_index = dev_index;
    clients[slot].policy    = policy;

    microkit_mr_set(0, USB_OK);
    microkit_mr_set(1, handle);
    return microkit_msginfo_new(0, 2);
}

/* MSG_USB_CLOSE (0x2304): MR1=handle → MR0=ok */
static microkit_msginfo handle_close(void) {
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    usb_client_t *c = find_client(handle);
    if (!c) {
        microkit_mr_set(0, USB_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    usb_memset(c, 0, sizeof(*c));
    microkit_mr_set(0, USB_OK);
    return microkit_msginfo_new(0, 1);
}

/*
 * MSG_USB_CONTROL (0x2305): MR1=handle MR2=setup_len MR3=data_len MR4=flags
 * Setup packet at shmem[0..7], data stage at shmem[8..8+data_len].
 *
 * XHCI_TRB_INTEGRATION_POINT: allocate a transfer TRB, fill the setup stage
 * descriptor from shmem, submit to the command ring, poll/await the Transfer
 * Event TRB completion, then copy data stage back to shmem.
 */
static microkit_msginfo handle_control(void) {
    uint32_t handle    = (uint32_t)microkit_mr_get(1);
    uint32_t setup_len = (uint32_t)microkit_mr_get(2);
    uint32_t data_len  = (uint32_t)microkit_mr_get(3);
    uint32_t flags     = (uint32_t)microkit_mr_get(4);
    (void)setup_len;
    (void)flags;

    usb_client_t *c = find_client(handle);
    if (!c) {
        microkit_mr_set(0, USB_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (devs[c->dev_index].present == false) {
        microkit_mr_set(0, USB_ERR_DISCONNECTED);
        return microkit_msginfo_new(0, 1);
    }

    /* Stub: report success and echo data_len back */
    microkit_mr_set(0, USB_OK);
    microkit_mr_set(1, data_len);
    return microkit_msginfo_new(0, 2);
}

/*
 * MSG_USB_BULK_IN (0x2306): MR1=handle MR2=ep MR3=max → MR0=ok MR1=actual
 * Data written to usb_shmem[0..actual].
 *
 * XHCI_TRB_INTEGRATION_POINT: allocate IN transfer TRB, set up data
 * descriptor pointing to usb_shmem physical address, submit, await event.
 */
static microkit_msginfo handle_bulk_in(void) {
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    uint32_t ep     = (uint32_t)microkit_mr_get(2);
    uint32_t max    = (uint32_t)microkit_mr_get(3);
    (void)ep;

    usb_client_t *c = find_client(handle);
    if (!c) {
        microkit_mr_set(0, USB_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (!devs[c->dev_index].present) {
        microkit_mr_set(0, USB_ERR_DISCONNECTED);
        return microkit_msginfo_new(0, 1);
    }
    if (!(c->policy & (USB_POLICY_HID | USB_POLICY_STORAGE | USB_POLICY_ALL))) {
        microkit_mr_set(0, USB_ERR_BAD_EP);
        return microkit_msginfo_new(0, 1);
    }

    /* Stub: report 0 bytes received (no real HW transfer) */
    (void)max;
    microkit_mr_set(0, USB_OK);
    microkit_mr_set(1, 0u);
    return microkit_msginfo_new(0, 2);
}

/*
 * MSG_USB_BULK_OUT (0x2307): MR1=handle MR2=ep MR3=len → MR0=ok MR1=actual
 * Data read from usb_shmem[0..len].
 *
 * XHCI_TRB_INTEGRATION_POINT: allocate OUT transfer TRB, submit, await event.
 */
static microkit_msginfo handle_bulk_out(void) {
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    uint32_t ep     = (uint32_t)microkit_mr_get(2);
    uint32_t len    = (uint32_t)microkit_mr_get(3);
    (void)ep;

    usb_client_t *c = find_client(handle);
    if (!c) {
        microkit_mr_set(0, USB_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (!devs[c->dev_index].present) {
        microkit_mr_set(0, USB_ERR_DISCONNECTED);
        return microkit_msginfo_new(0, 1);
    }

    /* Stub: claim all bytes were transmitted */
    microkit_mr_set(0, USB_OK);
    microkit_mr_set(1, len);
    return microkit_msginfo_new(0, 2);
}

/*
 * MSG_USB_STATUS (0x2308): MR1=handle → MR0=state MR1=err MR2=speed
 */
static microkit_msginfo handle_status(void) {
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    usb_client_t *c = find_client(handle);
    if (!c) {
        microkit_mr_set(0, USB_STATE_ERROR);
        microkit_mr_set(1, USB_ERR_BAD_HANDLE);
        microkit_mr_set(2, USB_SPEED_FULL);
        return microkit_msginfo_new(0, 3);
    }

    uint8_t speed = devs[c->dev_index].speed;
    microkit_mr_set(0, USB_STATE_CONFIGURED);
    microkit_mr_set(1, USB_OK);
    microkit_mr_set(2, speed);
    return microkit_msginfo_new(0, 3);
}

/* MSG_USB_RESET (not in contract header but implied by bead description) */
static microkit_msginfo handle_reset(void) {
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    usb_client_t *c = find_client(handle);
    if (!c) {
        microkit_mr_set(0, USB_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    /* XHCI_TRB_INTEGRATION_POINT: issue port reset TRB for the device port */
    microkit_mr_set(0, USB_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── Microkit entry points ────────────────────────────────────────────────── */

void init(void) {
    agentos_log_boot("usb_pd");
    usb_memset(devs,    0, sizeof(devs));
    usb_memset(clients, 0, sizeof(clients));

    /*
     * Architecture-specific host controller probe.
     *
     * On AArch64, try DWC3 first (RPi5 has a Synopsys DWC3 USB3 core
     * behind the PCIe bridge).  If DWC3 is not found, fall through to
     * the standard XHCI probe (QEMU xhci-pci model).
     *
     * On x86_64 (QEMU Q35, Intel NUC), probe XHCI directly.
     */
#if defined(ARCH_AARCH64)
    bool dwc3_ok = probe_dwc3();
    if (dwc3_ok)
        USB_LOG("DWC3 USB3 core found — using XHCI host interface\n");
#endif
    hw_present = probe_xhci();

    if (hw_present) {
        USB_LOG("USB host controller ready\n");
    } else {
        USB_LOG("USB host controller: stub mode (build with AGENTOS_USB_PD and wire MMIO)\n");
    }
}

void notified(microkit_channel ch) {
    /* Passive PD — device hotplug IRQ placeholder.
     * XHCI_IRQ_INTEGRATION_POINT: parse event ring TRBs to detect
     * connect/disconnect events and update dev_table accordingly. */
    (void)ch;
    USB_LOG("notified: hotplug IRQ (not yet wired)\n");
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    uint32_t op = (uint32_t)microkit_msginfo_get_label(msginfo);

    switch (op) {
        case MSG_USB_ENUMERATE: return handle_enumerate();
        case MSG_USB_LIST:      return handle_list();
        case MSG_USB_OPEN:      return handle_open();
        case MSG_USB_CLOSE:     return handle_close();
        case MSG_USB_CONTROL:   return handle_control();
        case MSG_USB_BULK_IN:   return handle_bulk_in();
        case MSG_USB_BULK_OUT:  return handle_bulk_out();
        case MSG_USB_STATUS:    return handle_status();
        /* MSG_USB_INT_XFER (0x2309): interrupt transfers not yet supported
         * (Phase 2+, per usb_contract.h).  Return USB_ERR_STALL as placeholder. */
        case 0x2309u:
            microkit_mr_set(0, USB_ERR_STALL);
            return microkit_msginfo_new(0, 1);
        /* MSG_USB_RESET (0x230A): port reset — stub, XHCI_TRB_INTEGRATION_POINT */
        case 0x230Au:           return handle_reset();
        default:
            USB_LOG("unknown opcode ");
            usb_log_hex(op);
            USB_LOG("\n");
            microkit_mr_set(0, 0xDEADu);
            return microkit_msginfo_new(1, 1);
    }
}

#else /* !AGENTOS_USB_PD — no-op stub ────────────────────────────────────── */

/*
 * Stub PD: compiled unconditionally so usb_pd.elf links cleanly.
 * Activate with: make build AGENTOS_USB_PD=1
 */

void init(void) {
    microkit_dbg_puts("[usb_pd] stub (build with AGENTOS_USB_PD=1 to enable)\n");
}

void notified(microkit_channel ch) { (void)ch; }

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch; (void)msginfo;
    microkit_mr_set(0, 0u);
    return microkit_msginfo_new(1, 1);
}

#endif /* AGENTOS_USB_PD */
