/*
 * dev_shell.c — agentOS Developer Shell Protection Domain
 *
 * Passive PD (priority 70, QEMU-only / #ifdef AGENTOS_DEV_BUILD).
 * Exposes a lightweight command REPL via the debug UART and a shared-memory
 * ring buffer.  Enables interactive debugging of agentOS on QEMU without
 * round-trip rebuild cycles.
 *
 * Architecture:
 *   dev_shell PD owns a 4KB ring buffer (dev_shell_ring MR) laid out as:
 *     [0..7]   magic (0xDE55E11) + version (1)
 *     [8..11]  cmd_head (host writes commands here)
 *     [12..15] cmd_tail (dev_shell reads from here)
 *     [16..2047] command ring (2032B circular, newline-terminated lines)
 *     [2048..2051] out_head (dev_shell writes output here)
 *     [2052..2055] out_tail (host reads from here)
 *     [2056..4095] output ring (2040B circular)
 *
 *   The RCC dashboard (or test harness) writes commands to the cmd ring
 *   and reads output from the out ring.  A Microkit notification wakes the
 *   dev_shell PD to process pending commands.
 *
 * Supported commands:
 *   help                        — list all commands
 *   pd list                     — list all registered PD names + priorities
 *   pd stat <id>                — stats for a specific PD (MR count, ch count)
 *   mem dump <mr_name> <off> <len> — hex dump bytes from a named MR
 *   ipc send <ch> <op> <arg>    — send a notify with MR0=op, MR1=arg
 *   trace dump                  — request a trace dump from trace_recorder
 *   fault inject <slot> <kind>  — trigger a fault in a slot (CI use)
 *   perf show                   — dump last perf export ring snapshot
 *   echo <text>                 — echo back text (loopback test)
 *   version                     — print agentOS version string
 *
 * IPC channels (from dev_shell's perspective):
 *   DEV_SHELL_HOST_CH  (0) — host notifies dev_shell of new commands
 *   DEV_SHELL_CTRL_CH  (1) — dev_shell → controller (for ipc send pass-through)
 *   DEV_SHELL_TRACE_CH (2) — dev_shell → trace_recorder
 *   DEV_SHELL_FAULT_CH (3) — dev_shell → fault_inject
 *
 * Build flag: only compiled when AGENTOS_DEV_BUILD is defined.
 * Not included in production agentos.system.  topology.yaml conditional:
 *   dev_shell PD has tag: dev-only (gen_sdf.py skips it unless --dev).
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#ifdef AGENTOS_DEV_BUILD

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Ring constants ───────────────────────────────────────────────────── */

#define DEV_SHELL_MAGIC       0xDE55E11UL
#define DEV_SHELL_VERSION     1
#define CMD_RING_OFFSET       16
#define CMD_RING_SIZE         2032
#define OUT_HDR_OFFSET        2048
#define OUT_RING_OFFSET       2056
#define OUT_RING_SIZE         2040
#define MAX_CMD_LEN           256
#define MAX_OUT_LEN           1024

/* ── IPC channels ─────────────────────────────────────────────────────── */

#define DEV_SHELL_HOST_CH     0
#define DEV_SHELL_CTRL_CH     1
#define DEV_SHELL_TRACE_CH    2
#define DEV_SHELL_FAULT_CH    3

/* ── Op codes for pass-through ────────────────────────────────────────── */

#define OP_TRACE_DUMP         0xA1
#define OP_FAULT_INJECT       0xF0

/* ── MR variables (set by Microkit linker from setvar_vaddr) ─────────── */

uintptr_t dev_shell_ring_vaddr;  /* The dev_shell_ring MR */

/* ── Ring accessors ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t cmd_head;
    volatile uint32_t cmd_tail;
    uint8_t  cmd_buf[CMD_RING_SIZE];
    volatile uint32_t out_head;
    volatile uint32_t out_tail;
    uint8_t  out_buf[OUT_RING_SIZE];
} dev_shell_ring_t;

_Static_assert(sizeof(dev_shell_ring_t) <= 4096,
               "dev_shell_ring_t must fit in 4KB MR");

static dev_shell_ring_t *s_ring = NULL;

static void ring_init(void) {
    if (!dev_shell_ring_vaddr) return;
    s_ring = (dev_shell_ring_t *)dev_shell_ring_vaddr;
    memset(s_ring, 0, sizeof(dev_shell_ring_t));
    s_ring->magic   = DEV_SHELL_MAGIC;
    s_ring->version = DEV_SHELL_VERSION;
    __asm__ volatile("dmb ish" ::: "memory");
}

/* Read one complete command line (up to '\n') from cmd ring.
 * Returns true and fills buf (null-terminated, '\n' stripped). */
static bool cmd_read_line(char *buf, int max) {
    if (!s_ring) return false;
    int n = 0;
    while (n < max - 1) {
        uint32_t head = s_ring->cmd_head;
        uint32_t tail = s_ring->cmd_tail;
        if (head == tail) {
            /* No more data — partial line in buffer, wait for more */
            return false;
        }
        uint8_t c = s_ring->cmd_buf[tail % CMD_RING_SIZE];
        s_ring->cmd_tail = tail + 1;
        if (c == '\n' || c == '\r') {
            break;
        }
        buf[n++] = (char)c;
    }
    buf[n] = '\0';
    return (n > 0);
}

/* Write output string to the output ring (overwrites on overflow). */
static void out_write(const char *str) {
    if (!s_ring || !str) return;
    for (const char *p = str; *p; p++) {
        uint32_t head = s_ring->out_head;
        s_ring->out_buf[head % OUT_RING_SIZE] = (uint8_t)*p;
        __asm__ volatile("dmb ishst" ::: "memory");
        s_ring->out_head = head + 1;
    }
    __asm__ volatile("dmb ish" ::: "memory");
}

/* Simple decimal integer formatter (no libc sprintf in bare metal PD). */
static void out_uint(uint32_t val) {
    char tmp[12];
    int i = 0;
    if (val == 0) { out_write("0"); return; }
    while (val && i < 11) { tmp[i++] = '0' + (val % 10); val /= 10; }
    for (int j = i - 1; j >= 0; j--) { char c = tmp[j]; out_write(&c); /* single char */ }
    /* Cleaner: write reversed */
    char rev[12]; int k = 0;
    for (int j = i - 1; j >= 0; j--) rev[k++] = tmp[j];
    rev[k] = '\0';
    out_write(rev);
}

/* Hex byte formatter */
static const char hex_chars[] = "0123456789abcdef";
static void out_hex_byte(uint8_t b) {
    char tmp[3] = { hex_chars[b >> 4], hex_chars[b & 0xF], '\0' };
    out_write(tmp);
}

/* ── Known MR registry (compile-time, matches topology.yaml) ─────────── */

typedef struct { const char *name; uintptr_t *vaddr_ptr; size_t size; } MREntry;

extern uintptr_t perf_ring_vaddr;
extern uintptr_t mem_ring_vaddr;
extern uintptr_t vibe_code_vaddr;
extern uintptr_t vibe_state_vaddr;
extern uintptr_t gpu_tensor_buf_vaddr;

static const MREntry known_mrs[] = {
    { "perf_ring",      &perf_ring_vaddr,      0x1000   },
    { "mem_ring",       &mem_ring_vaddr,        0x40000  },
    { "vibe_code",      &vibe_code_vaddr,       0x400000 },
    { "vibe_state",     &vibe_state_vaddr,      0x10000  },
    { "gpu_tensor_buf", &gpu_tensor_buf_vaddr,  0x4000000},
    { "dev_shell_ring", &dev_shell_ring_vaddr,  0x1000   },
};
#define KNOWN_MR_COUNT (sizeof(known_mrs)/sizeof(known_mrs[0]))

/* ── Command handlers ─────────────────────────────────────────────────── */

static void cmd_help(void) {
    out_write("agentOS dev_shell — commands:\r\n"
              "  help\r\n"
              "  version\r\n"
              "  echo <text>\r\n"
              "  pd list\r\n"
              "  pd stat <id>\r\n"
              "  mem dump <mr_name> <offset_hex> <len_dec>\r\n"
              "  ipc send <ch_id> <op_hex> <arg_hex>\r\n"
              "  trace dump\r\n"
              "  fault inject <slot_id> <kind_hex>\r\n"
              "  perf show\r\n"
              "  mr list\r\n");
}

static void cmd_version(void) {
    out_write(AGENTOS_VERSION_STR "\r\n");
}

static void cmd_echo(const char *args) {
    out_write(args);
    out_write("\r\n");
}

static void cmd_pd_list(void) {
    /* In a real implementation this would PPC the controller for the PD table.
     * For the prototype we emit a static list from topology knowledge. */
    out_write("PD list (static, QEMU build):\r\n"
              "  0  controller        pri=50\r\n"
              "  1  swap_slot_0       pri=75\r\n"
              "  2  swap_slot_1       pri=75\r\n"
              "  3  swap_slot_2       pri=75\r\n"
              "  4  swap_slot_3       pri=75\r\n"
              "  5  worker_0..7       pri=80\r\n"
              "  6  init_agent        pri=90\r\n"
              "  7  fault_inject      pri=85\r\n"
              "  8  mem_profiler      pri=110\r\n"
              "  9  event_bus         pri=120\r\n"
              " 10  trace_recorder    pri=130\r\n"
              " 11  vibe_engine       pri=140\r\n"
              " 12  agentfs           pri=150\r\n"
              " 13  console_mux       pri=160\r\n"
              " 14  linux_vmm         pri=100\r\n"
              " 15  dev_shell         pri=70  (this PD)\r\n");
}

static void cmd_pd_stat(const char *args) {
    out_write("pd stat: PPC to controller not yet wired (TODO)\r\n");
    out_write("pd_id=");
    out_write(args);
    out_write("\r\n");
}

static void cmd_mr_list(void) {
    out_write("Known MRs:\r\n");
    for (size_t i = 0; i < KNOWN_MR_COUNT; i++) {
        out_write("  ");
        out_write(known_mrs[i].name);
        out_write("  vaddr=");
        /* Print vaddr as hex */
        uintptr_t v = *known_mrs[i].vaddr_ptr;
        out_write("0x");
        for (int s = 28; s >= 0; s -= 4) {
            char c = hex_chars[(v >> s) & 0xF];
            out_write(&c);
        }
        out_write("  size=0x");
        uint32_t sz = (uint32_t)known_mrs[i].size;
        for (int s = 28; s >= 0; s -= 4) {
            char c = hex_chars[(sz >> s) & 0xF];
            out_write(&c);
        }
        out_write("\r\n");
    }
}

static void cmd_mem_dump(const char *args) {
    /* Parse: <mr_name> <offset_hex> <len_dec> */
    char mr_name[64] = {0};
    unsigned long offset = 0, len = 0;
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) mr_name[i++] = *p++;
    while (*p == ' ') p++;
    /* Parse hex offset */
    if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) p += 2;
    while (*p && *p != ' ') {
        offset = offset * 16 + (*p >= 'a' ? *p - 'a' + 10 : *p >= 'A' ? *p - 'A' + 10 : *p - '0');
        p++;
    }
    while (*p == ' ') p++;
    while (*p) { len = len * 10 + (*p - '0'); p++; }

    /* Find MR */
    const MREntry *mr = NULL;
    for (size_t k = 0; k < KNOWN_MR_COUNT; k++) {
        if (strcmp(known_mrs[k].name, mr_name) == 0) { mr = &known_mrs[k]; break; }
    }
    if (!mr) { out_write("unknown MR: "); out_write(mr_name); out_write("\r\n"); return; }
    if (!*mr->vaddr_ptr) { out_write("MR not mapped in this PD\r\n"); return; }
    if (len == 0 || len > 256) len = 64;
    if (offset + len > mr->size) { out_write("offset+len exceeds MR size\r\n"); return; }

    const uint8_t *base = (const uint8_t *)(*mr->vaddr_ptr + offset);
    for (unsigned long row = 0; row < len; row += 16) {
        out_write("  ");
        out_hex_byte((uint8_t)((offset + row) >> 8));
        out_hex_byte((uint8_t)(offset + row));
        out_write(": ");
        for (unsigned long col = 0; col < 16 && row + col < len; col++) {
            out_hex_byte(base[row + col]);
            out_write(" ");
        }
        out_write("\r\n");
    }
}

static void cmd_ipc_send(const char *args) {
    /* Parse: <ch_id> <op_hex> <arg_hex> */
    unsigned long ch = 0, op = 0, arg = 0;
    const char *p = args;
    while (*p && *p != ' ') { ch = ch * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) p += 2;
    while (*p && *p != ' ') {
        op = op * 16 + (*p >= 'a' ? *p - 'a' + 10 : *p >= 'A' ? *p - 'A' + 10 : *p - '0');
        p++;
    }
    while (*p == ' ') p++;
    if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) p += 2;
    while (*p) {
        arg = arg * 16 + (*p >= 'a' ? *p - 'a' + 10 : *p >= 'A' ? *p - 'A' + 10 : *p - '0');
        p++;
    }
    /* Only allow pass-through to DEV_SHELL_CTRL_CH for safety */
    if (ch != DEV_SHELL_CTRL_CH) {
        out_write("ipc send: only ch=");
        out_uint(DEV_SHELL_CTRL_CH);
        out_write(" (controller) allowed in dev_shell\r\n");
        return;
    }
    microkit_mr_set(0, (seL4_Word)op);
    microkit_mr_set(1, (seL4_Word)arg);
    microkit_notify((microkit_channel)ch);
    out_write("ipc send: notified ch=");
    out_uint((uint32_t)ch);
    out_write(" op=0x");
    out_hex_byte((uint8_t)op);
    out_write(" arg=0x");
    out_hex_byte((uint8_t)arg);
    out_write("\r\n");
}

static void cmd_trace_dump(void) {
    microkit_mr_set(0, OP_TRACE_DUMP);
    microkit_notify(DEV_SHELL_TRACE_CH);
    out_write("trace dump: notified trace_recorder\r\n");
}

static void cmd_fault_inject(const char *args) {
    unsigned long slot = 0, kind = 0;
    const char *p = args;
    while (*p && *p != ' ') { slot = slot * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) p += 2;
    while (*p) {
        kind = kind * 16 + (*p >= 'a' ? *p - 'a' + 10 : *p >= 'A' ? *p - 'A' + 10 : *p - '0');
        p++;
    }
    microkit_mr_set(0, OP_FAULT_INJECT);
    microkit_mr_set(1, (seL4_Word)slot);
    microkit_mr_set(2, (seL4_Word)kind);
    microkit_notify(DEV_SHELL_FAULT_CH);
    out_write("fault inject: slot=");
    out_uint((uint32_t)slot);
    out_write(" kind=0x");
    out_hex_byte((uint8_t)kind);
    out_write(" → notified fault_inject PD\r\n");
}

static void cmd_perf_show(void) {
    out_write("perf show: reading perf_ring MR...\r\n");
    /* Minimal: dump first 4 records from perf_ring */
    extern uintptr_t perf_ring_vaddr;
    if (!perf_ring_vaddr) { out_write("  perf_ring not mapped\r\n"); return; }
    typedef struct { uint64_t ts_ns; uint8_t pd_id; uint8_t ch; uint64_t lat; uint8_t _p[6]; } PRec;
    const PRec *ring = (const PRec *)perf_ring_vaddr;
    for (int i = 0; i < 4; i++) {
        out_write("  [");
        out_uint((uint32_t)i);
        out_write("] pd=");
        out_uint(ring[i].pd_id);
        out_write(" ch=");
        out_uint(ring[i].ch);
        out_write(" lat=");
        out_uint((uint32_t)ring[i].lat);
        out_write("ns\r\n");
    }
}

/* ── Command dispatcher ───────────────────────────────────────────────── */

static void dispatch(const char *line) {
    if (!line || !*line) return;

    /* Strip leading whitespace */
    while (*line == ' ') line++;

    if (strcmp(line, "help") == 0)           { cmd_help(); return; }
    if (strcmp(line, "version") == 0)        { cmd_version(); return; }
    if (strncmp(line, "echo ", 5) == 0)      { cmd_echo(line + 5); return; }
    if (strcmp(line, "pd list") == 0)        { cmd_pd_list(); return; }
    if (strncmp(line, "pd stat ", 8) == 0)   { cmd_pd_stat(line + 8); return; }
    if (strcmp(line, "mr list") == 0)        { cmd_mr_list(); return; }
    if (strncmp(line, "mem dump ", 9) == 0)  { cmd_mem_dump(line + 9); return; }
    if (strncmp(line, "ipc send ", 9) == 0)  { cmd_ipc_send(line + 9); return; }
    if (strcmp(line, "trace dump") == 0)     { cmd_trace_dump(); return; }
    if (strncmp(line, "fault inject ", 13) == 0) { cmd_fault_inject(line + 13); return; }
    if (strcmp(line, "perf show") == 0)      { cmd_perf_show(); return; }

    out_write("unknown command: ");
    out_write(line);
    out_write("\r\nType 'help' for command list.\r\n");
}

/* ── Microkit entry points ────────────────────────────────────────────── */

void init(void) {
    ring_init();
    out_write("\r\nagentOS dev_shell ready. Type 'help' for commands.\r\n> ");
}

void notified(microkit_channel ch) {
    if (ch == DEV_SHELL_HOST_CH) {
        char line[MAX_CMD_LEN];
        while (cmd_read_line(line, MAX_CMD_LEN)) {
            dispatch(line);
            out_write("> ");
        }
    }
}

seL4_Bool fault(microkit_child child, microkit_msginfo msginfo,
                microkit_msginfo *reply_msginfo) {
    (void)child; (void)msginfo; (void)reply_msginfo;
    out_write("dev_shell: unexpected fault (this PD should not fault)\r\n");
    return seL4_False;
}

#endif /* AGENTOS_DEV_BUILD */
