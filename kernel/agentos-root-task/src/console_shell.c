/*
 * console_shell.c — agentOS Management Console Shell
 *
 * Active PD (priority 80).  Provides an operator-facing serial console for
 * loading and starting VMs, configuring networking, and querying status.
 *
 * ── Input modes ──────────────────────────────────────────────────────────────
 *
 * Ring buffer (QEMU / bridge.rs dev mode)
 *   bridge.rs POSTs commands to POST /api/agentos/console/cmd → writes to
 *   console_shell_ring.cmd_buf → notifies CH_HOST (0).
 *   Available when console_shell_ring_vaddr is mapped.
 *
 * Direct UART (native hardware: intel-nuc, rpi5)
 *   When built with -DBOARD_UART_PL011 or -DBOARD_UART_NS16550, the PD drives
 *   the physical UART MMIO directly.  UART RX interrupt fires on CH_UART_IRQ (3).
 *   PL011 (ARM):     -DBOARD_UART_PL011   (aarch64 boards)
 *   NS16550 MMIO:    -DBOARD_UART_NS16550 (x86 LPSS UART, RISC-V NS16550A)
 *
 * ── Output path ──────────────────────────────────────────────────────────────
 *
 *   Ring out_buf (bridge.rs reads via GET /api/agentos/console/stream)
 *   + microkit_dbg_puts() (serial socket on QEMU, debug UART on hardware)
 *
 * ── VM management: QEMU vs native ────────────────────────────────────────────
 *
 * QEMU dev mode (-DBOARD_NATIVE not set):
 *   VMs are launched by bridge.rs (host process).  console_shell emits a
 *   serial escape to signal bridge.rs:  "\x01VM:start:freebsd\n"
 *   bridge.rs detects these sequences from the serial socket and acts on them.
 *
 * Native hardware (-DBOARD_NATIVE=1):
 *   VMs run as seL4 PDs (linux_vmm, freebsd_vmm).  console_shell PPCs the
 *   controller on CH_CTRL (1) with OP_VM_CREATE / OP_VM_DESTROY.
 *
 * ── IPC channels ─────────────────────────────────────────────────────────────
 *
 *   CH 0  (CH_HOST)     ring mode: bridge/host signals new input
 *   CH 1  (CH_CTRL)     console_shell → controller (VM mgmt PPC, native only)
 *   CH 2  (CH_NET)      console_shell ↔ net_server (future)
 *   CH 3  (CH_UART_IRQ) UART RX interrupt (native hardware with UART type set)
 *
 * ── Supported commands ───────────────────────────────────────────────────────
 *
 *   help                       show this list
 *   version                    print system version
 *   status                     PD summary + running VMs
 *   vm list                    list VMs and their state
 *   vm start <id>              start a VM (freebsd | ubuntu)
 *   vm stop  <id>              stop a running VM
 *   vm console <id>            UART passthrough to VM serial (native; Ctrl-] exits)
 *   net info                   show network configuration
 *   net up dhcp                configure NIC via DHCP
 *   net up <ip/prefix> <gw>    configure static IP
 *   reboot                     warm reboot
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Channel IDs ─────────────────────────────────────────────────────────── */

#define CH_HOST       0u   /* ring: host/bridge notifies of new input       */
#define CH_CTRL       1u   /* controller PPC (VM mgmt, native hardware)     */
#define CH_NET        2u   /* net_server (future)                           */
#define CH_UART_IRQ   3u   /* UART RX interrupt (native hardware only)      */

/* ── Ring buffer ─────────────────────────────────────────────────────────── */

#define SHELL_MAGIC   0xC5E11UL
#define CMD_RING_SIZE 2032u
#define OUT_RING_SIZE 2040u
#define MAX_LINE      256

typedef struct {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t cmd_head;
    volatile uint32_t cmd_tail;
    uint8_t  cmd_buf[CMD_RING_SIZE];
    volatile uint32_t out_head;
    volatile uint32_t out_tail;
    uint8_t  out_buf[OUT_RING_SIZE];
} shell_ring_t;

_Static_assert(sizeof(shell_ring_t) <= 4096, "shell_ring_t must fit in 4KB");

/* Linker-set MR vaddrs */
uintptr_t console_shell_ring_vaddr;   /* 4KB ring MR (ring mode)           */
uintptr_t console_uart_vaddr;          /* UART MMIO base (native HW)        */

static shell_ring_t *s_ring = NULL;

/* ── UART driver ─────────────────────────────────────────────────────────── */

#if defined(BOARD_UART_PL011)

#define PL011_DR   0x00u
#define PL011_FR   0x18u
#define PL011_RXFE (1u << 4)
#define PL011_TXFF (1u << 5)

static inline uint32_t pl011_rd(uint32_t off) {
    return *(volatile uint32_t *)(console_uart_vaddr + off);
}
static inline void pl011_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(console_uart_vaddr + off) = v;
}

static void     uart_hw_init(void) { /* enabled by firmware */ }
static bool     uart_rx_avail(void) { return !(pl011_rd(PL011_FR) & PL011_RXFE); }
static uint8_t  uart_rx_byte(void)  { return (uint8_t)(pl011_rd(PL011_DR) & 0xFF); }
static void uart_tx_byte(uint8_t c) {
    while (pl011_rd(PL011_FR) & PL011_TXFF) {}
    pl011_wr(PL011_DR, c);
}
#define UART_HW 1

#elif defined(BOARD_UART_NS16550)

#define NS_RBR  0x00u
#define NS_THR  0x00u
#define NS_LSR  0x14u
#define NS_DR   (1u << 0)
#define NS_THRE (1u << 5)

static inline uint8_t ns_rd(uint32_t off) {
    return *(volatile uint8_t *)(console_uart_vaddr + off);
}
static inline void ns_wr(uint32_t off, uint8_t v) {
    *(volatile uint8_t *)(console_uart_vaddr + off) = v;
}

static void     uart_hw_init(void) {}
static bool     uart_rx_avail(void) { return !!(ns_rd(NS_LSR) & NS_DR); }
static uint8_t  uart_rx_byte(void)  { return ns_rd(NS_RBR); }
static void uart_tx_byte(uint8_t c) {
    while (!(ns_rd(NS_LSR) & NS_THRE)) {}
    ns_wr(NS_THR, c);
}
#define UART_HW 1

#else  /* no hardware UART — ring-only */

static void     uart_hw_init(void) {}
static bool     uart_rx_avail(void) { (void)console_uart_vaddr; return false; }
static uint8_t  uart_rx_byte(void)  { return 0; }
static void     uart_tx_byte(uint8_t c) { (void)c; }

#endif

/* Write a string to both the out ring and the debug UART */
static void emit(const char *s) {
    if (s_ring) {
        for (const char *p = s; *p; p++) {
            uint32_t h = s_ring->out_head;
            s_ring->out_buf[h % OUT_RING_SIZE] = (uint8_t)*p;
            __asm__ volatile("" ::: "memory");
            s_ring->out_head = h + 1;
        }
    }
    microkit_dbg_puts(s);
#ifdef UART_HW
    for (const char *p = s; *p; p++) {
        if (*p == '\n') uart_tx_byte('\r');
        uart_tx_byte((uint8_t)*p);
    }
#endif
}

static void emit_uint(uint32_t v) {
    char tmp[12]; int n = 0;
    if (!v) { emit("0"); return; }
    while (v && n < 11) { tmp[n++] = '0' + (int)(v % 10); v /= 10; }
    char buf[12]; int k = 0;
    for (int i = n - 1; i >= 0; i--) buf[k++] = tmp[i];
    buf[k] = '\0'; emit(buf);
}

/* ── Line editor (UART interactive mode) ─────────────────────────────────── */

static char s_line[MAX_LINE];
static int  s_linelen;
static bool s_passthrough;

/* Push a received byte through the line editor.
 * Returns true when a complete command line is ready in buf. */
static bool lineed(uint8_t c, char *buf) {
    if (s_passthrough) {
        if (c == 0x1D) {   /* Ctrl-] exits passthrough */
            s_passthrough = false;
            emit("\r\n[console_shell] exited VM console\r\n> ");
        }
        /* TODO: forward byte to VM via IPC */
        return false;
    }
    if (c == '\r' || c == '\n') {
        s_line[s_linelen] = '\0';
        if (s_linelen > 0) {
            memcpy(buf, s_line, (size_t)(s_linelen + 1));
            s_linelen = 0;
            emit("\r\n");
            return true;
        }
        emit("\r\n> ");
        return false;
    }
    if (c == 0x7F || c == '\b') {
        if (s_linelen > 0) { s_linelen--; emit("\b \b"); }
        return false;
    }
    if (c == 0x15) { while (s_linelen > 0) { emit("\b \b"); s_linelen--; } return false; }
    if (c < 0x20)  return false;
    if (s_linelen < MAX_LINE - 1) {
        char echo[2] = { (char)c, '\0' };
        s_line[s_linelen++] = (char)c;
        emit(echo);
    }
    return false;
}

/* ── Ring input reader ────────────────────────────────────────────────────── */

static bool ring_readline(char *buf, int max) {
    if (!s_ring) return false;
    int n = 0;
    while (n < max - 1) {
        uint32_t h = s_ring->cmd_head, t = s_ring->cmd_tail;
        if (h == t) break;
        uint8_t c = s_ring->cmd_buf[t % CMD_RING_SIZE];
        s_ring->cmd_tail = t + 1;
        if (c == '\n' || c == '\r') { if (n > 0) break; continue; }
        buf[n++] = (char)c;
    }
    buf[n] = '\0';
    return (n > 0);
}

/* ── VM backend: QEMU vs native ──────────────────────────────────────────── */

/*
 * On QEMU, bridge.rs manages VM processes.  Communicate via a serial escape
 * sequence that bridge.rs parses from the serial socket output:
 *   "\x01VM:start:freebsd\n"  → bridge.rs spawns FreeBSD QEMU process
 *   "\x01VM:stop:freebsd\n"   → bridge.rs kills FreeBSD QEMU process
 *
 * On native hardware (BOARD_NATIVE defined), PPCs the controller.
 */
#ifndef OP_VM_CREATE
#define OP_VM_CREATE  0x10u
#endif
#ifndef OP_VM_DESTROY
#define OP_VM_DESTROY 0x11u
#endif
/* OP_VM_LIST: → MR0=ok MR1=running_bitmask (freebsd=bit0, ubuntu=bit1) */
#ifndef OP_VM_LIST
#define OP_VM_LIST    0x18u
#endif
#define OP_VM_STATUS  OP_VM_LIST   /* alias used in vm_list() */

static void vm_start(const char *id) {
#ifdef BOARD_NATIVE
    uint64_t atype = (strncmp(id, "ubuntu", 6) == 0) ? 1u : 0u;
    microkit_mr_set(0, OP_VM_CREATE);
    microkit_mr_set(1, atype);
    microkit_msginfo r = microkit_ppcall(CH_CTRL, microkit_msginfo_new(OP_VM_CREATE, 2));
    uint64_t ok = microkit_mr_get(0);
    (void)r;
    emit(ok ? "  VM starting.\r\n" : "  Failed — check system log.\r\n");
#else
    /* QEMU dev mode: signal bridge.rs via serial escape */
    emit("\x01VM:start:"); emit(id); emit("\n");
    emit("  Requested — bridge.rs will launch the VM.\r\n");
    emit("  SSH info visible in the web dashboard (VM panel).\r\n");
#endif
}

static void vm_stop(const char *id) {
#ifdef BOARD_NATIVE
    uint64_t atype = (strncmp(id, "ubuntu", 6) == 0) ? 1u : 0u;
    microkit_mr_set(0, OP_VM_DESTROY);
    microkit_mr_set(1, atype);
    microkit_msginfo r = microkit_ppcall(CH_CTRL, microkit_msginfo_new(OP_VM_DESTROY, 2));
    (void)r;
    emit("  VM stopped.\r\n");
#else
    emit("\x01VM:stop:"); emit(id); emit("\n");
    emit("  Requested — bridge.rs will stop the VM.\r\n");
#endif
}

static void vm_list(void) {
#ifdef BOARD_NATIVE
    microkit_mr_set(0, OP_VM_STATUS);
    microkit_mr_set(1, 0xFFu);
    microkit_msginfo r = microkit_ppcall(CH_CTRL, microkit_msginfo_new(OP_VM_STATUS, 2));
    uint64_t mask = microkit_mr_get(0);
    (void)r;
    emit("  freebsd : "); emit((mask & 1u) ? "running" : "stopped"); emit("\r\n");
    emit("  ubuntu  : "); emit((mask & 2u) ? "running" : "stopped"); emit("\r\n");
#else
    emit("\x01VM:list\n");
    emit("  (VM status visible in web dashboard — bridge.rs manages VMs on QEMU)\r\n");
    emit("  freebsd  SSH port 2222 | ubuntu  SSH port 2223\r\n");
    emit("  Passwords: agentos\r\n");
#endif
}

/* ── Command handlers ─────────────────────────────────────────────────────── */

static void cmd_help(void) {
    emit(
        "\r\nagentOS console_shell — board management interface\r\n"
        "  help                    show this list\r\n"
        "  version                 print system version\r\n"
        "  status                  system + VM summary\r\n"
        "  vm list                 list VMs and their state\r\n"
        "  vm start freebsd        start FreeBSD VM\r\n"
        "  vm start ubuntu         start Ubuntu VM\r\n"
        "  vm stop  <freebsd|ubuntu>  stop a running VM\r\n"
#ifdef UART_HW
        "  vm console <id>         enter VM serial console (Ctrl-] to exit)\r\n"
#endif
        "  net info                show network configuration\r\n"
        "  net up dhcp             configure NIC via DHCP\r\n"
        "  net up <ip/prefix> <gw> configure static IP\r\n"
        "  reboot                  warm reboot\r\n"
        "\r\n");
}

static void cmd_version(void) {
    emit("agentOS console_shell v0.1 (" AGENTOS_VERSION_STR ")\r\n");
#ifdef BOARD_NATIVE
    emit("Mode: native hardware\r\n");
#else
    emit("Mode: QEMU dev (bridge.rs manages VMs)\r\n");
#endif
}

static void cmd_status(void) {
    emit("agentOS system status\r\n");
    emit("  Version : " AGENTOS_VERSION_STR "\r\n");
    emit("  Ring    : ");
    emit(console_shell_ring_vaddr ? "active\r\n" : "not mapped\r\n");
    emit("  UART    : ");
#if defined(BOARD_UART_PL011)
    emit("PL011 MMIO\r\n");
#elif defined(BOARD_UART_NS16550)
    emit("NS16550 MMIO\r\n");
#else
    emit("ring-only (QEMU dev mode)\r\n");
#endif
    emit("VMs:\r\n");
    vm_list();
}

static void cmd_net_info(void) {
    emit("Network configuration:\r\n");
#ifndef BOARD_NATIVE
    emit("  Mode:    QEMU SLIRP (user network)\r\n");
    emit("  Gateway: 10.0.2.2  DNS: 10.0.2.3\r\n");
    emit("  Web UI:  http://localhost:8080  (bridge.rs)\r\n");
    emit("  SSH:     FreeBSD=2222  Ubuntu=2223  (hostfwd)\r\n");
#else
    emit("  (net_server SDDF integration — query via CH_NET, not yet wired)\r\n");
    emit("  Run 'net up dhcp' or 'net up <ip/prefix> <gw>' to configure.\r\n");
#endif
}

static void cmd_net_up(const char *args) {
    if (strncmp(args, "dhcp", 4) == 0) {
        emit("Requesting DHCP...\r\n");
        /* TODO: send OP_NET_DHCP to net_server via CH_NET */
        emit("  (net_server DHCP not yet wired — see net_server.c)\r\n");
    } else {
        emit("Setting static IP: "); emit(args); emit("\r\n");
        emit("  (net_server static config not yet wired)\r\n");
    }
}

static void cmd_reboot(void) {
    emit("Rebooting...\r\n");
    microkit_notify(CH_CTRL);   /* ask controller to trigger platform reset */
    for (volatile int i = 0; i < 2000000; i++) {}
    emit("  (reboot not available — halting)\r\n");
}

/* ── Command dispatcher ──────────────────────────────────────────────────── */

static void dispatch(const char *line) {
    if (!line || !*line) { emit("> "); return; }
    while (*line == ' ') line++;

    if      (strcmp(line, "help")              == 0) cmd_help();
    else if (strcmp(line, "version")           == 0) cmd_version();
    else if (strcmp(line, "status")            == 0) cmd_status();
    else if (strcmp(line, "vm list")           == 0) vm_list();
    else if (strncmp(line, "vm start ", 9)     == 0) vm_start(line + 9);
    else if (strncmp(line, "vm stop ",  8)     == 0) vm_stop(line + 8);
#ifdef UART_HW
    else if (strncmp(line, "vm console ", 11)  == 0) {
        emit("Entering VM console for '"); emit(line + 11);
        emit("'\r\n  Press Ctrl-] to return to console_shell.\r\n");
        s_passthrough = true;
    }
#endif
    else if (strcmp(line, "net info")          == 0) cmd_net_info();
    else if (strncmp(line, "net up ", 7)       == 0) cmd_net_up(line + 7);
    else if (strcmp(line, "reboot")            == 0) cmd_reboot();
    else {
        emit("unknown: "); emit(line);
        emit("\r\n  Type 'help' for commands.\r\n");
    }
    emit("> ");
}

/* ── UART RX drain (native hardware UART IRQ handler) ────────────────────── */
#ifdef UART_HW
static void uart_drain(void) {
    char buf[MAX_LINE];
    while (uart_rx_avail()) {
        if (lineed(uart_rx_byte(), buf)) dispatch(buf);
    }
}
#endif

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void) {
    if (console_shell_ring_vaddr) {
        s_ring = (shell_ring_t *)console_shell_ring_vaddr;
        s_ring->magic    = SHELL_MAGIC;
        s_ring->version  = 1;
        s_ring->cmd_head = s_ring->cmd_tail = 0;
        s_ring->out_head = s_ring->out_tail = 0;
        __asm__ volatile("" ::: "memory");
    }
    uart_hw_init();

    microkit_dbg_puts("[console_shell] PD online\n");
    emit("\r\n"
         "  agentOS console_shell — type 'help' for commands\r\n"
         "> ");
}

void notified(microkit_channel ch) {
    char buf[MAX_LINE];

    if (ch == CH_HOST) {
        while (ring_readline(buf, MAX_LINE)) dispatch(buf);
    }
#ifdef UART_HW
    if (ch == CH_UART_IRQ) uart_drain();
#endif
    if (ch == CH_CTRL) {
        emit("\r\n[VM state changed — run 'vm list']\r\n> ");
    }
    (void)buf;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch; (void)msginfo;
    microkit_mr_set(0, 0xDEAD);
    return microkit_msginfo_new(0, 1);
}
