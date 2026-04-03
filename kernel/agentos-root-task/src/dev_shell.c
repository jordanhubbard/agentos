/*
 * dev_shell.c — agentOS Developer Shell Protection Domain
 *
 * Active PD (priority 70, QEMU-only / #ifdef AGENTOS_DEV_SHELL).
 * Exposes a lightweight command REPL via the debug UART and a shared-memory
 * ring buffer.  Enables interactive debugging of agentOS on QEMU without
 * round-trip rebuild cycles.
 *
 * Architecture:
 *   dev_shell PD owns a 4KB ring buffer (dev_shell_ring MR) laid out as:
 *     [0..7]     magic (0xDE55E11) + version (1)
 *     [8..11]    cmd_head (host writes commands here)
 *     [12..15]   cmd_tail (dev_shell reads from here)
 *     [16..2047] command ring (2032B circular, newline-terminated lines)
 *     [2048..2051] out_head (dev_shell writes output here)
 *     [2052..2055] out_tail (host reads from here)
 *     [2056..4095] output ring (2040B circular)
 *
 *   The agentOS console (or test harness) writes commands to the cmd ring
 *   and reads output from the out ring.  A Microkit notification wakes the
 *   dev_shell PD to process pending commands.
 *
 * Supported commands:
 *   help
 *   version
 *   echo <text>
 *   pd list
 *   pd stat <id>
 *   mem dump <mr_name> <offset_hex> <len_dec>
 *   ipc send <ch_id> <op_hex> <arg_hex>
 *   trace dump
 *   fault inject <slot_id> <kind:vm|cap|null>
 *   perf show
 *   mr list
 *   quit                               (no-op; shell stays running)
 *
 * IPC channels (from dev_shell's perspective):
 *   DEV_SHELL_HOST_CH  (0) — host notifies dev_shell of new commands
 *   DEV_SHELL_CTRL_CH  (1) — dev_shell → controller (for ipc send pass-through)
 *   DEV_SHELL_TRACE_CH (2) — dev_shell → trace_recorder
 *   DEV_SHELL_FAULT_CH (3) — dev_shell → fault_inject
 *
 * Build flag: compiled with full logic when AGENTOS_DEV_SHELL is defined
 * (pass -DAGENTOS_DEV_SHELL or use `make dev-shell`); otherwise a no-op stub
 * keeps the system image valid in every build.
 *
 * agentOS console integration: see services/dev-shell/README.md
 *   GET  /api/agentos/shell     — SSE stream of dev_shell output ring
 *   POST /api/agentos/shell/cmd — write command to cmd ring, notify dev_shell
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef AGENTOS_DEV_SHELL

/* ── Ring constants ───────────────────────────────────────────────────────── */

#define DEV_SHELL_MAGIC       0xDE55E11UL
#define DEV_SHELL_VERSION     1
#define CMD_RING_SIZE         2032
#define OUT_RING_SIZE         2040
#define MAX_CMD_LEN           256
#define MAX_OUT_LEN           1024

/* ── IPC channels ─────────────────────────────────────────────────────────── */

#define DEV_SHELL_HOST_CH     0
#define DEV_SHELL_CTRL_CH     1
#define DEV_SHELL_TRACE_CH    2
#define DEV_SHELL_FAULT_CH    3

/* ── Op codes for pass-through ────────────────────────────────────────────── */

#define OP_TRACE_DUMP         0xA1

/* ── MR variable (set by Microkit linker via setvar_vaddr) ────────────────── */

uintptr_t dev_shell_ring_vaddr;

/* ── Ring layout ──────────────────────────────────────────────────────────── */

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
    __asm__ volatile("" ::: "memory");
}

/* Read one complete command line (up to '\n') from the cmd ring.
 * Returns true and fills buf (null-terminated, '\n' stripped). */
static bool cmd_read_line(char *buf, int max) {
    if (!s_ring) return false;
    int n = 0;
    while (n < max - 1) {
        uint32_t head = s_ring->cmd_head;
        uint32_t tail = s_ring->cmd_tail;
        if (head == tail) return false;   /* no more data */
        uint8_t c = s_ring->cmd_buf[tail % CMD_RING_SIZE];
        s_ring->cmd_tail = tail + 1;
        if (c == '\n' || c == '\r') break;
        buf[n++] = (char)c;
    }
    buf[n] = '\0';
    return (n > 0);
}

/* Write string to the output ring. */
static void out_write(const char *str) {
    if (!s_ring || !str) return;
    for (const char *p = str; *p; p++) {
        uint32_t head = s_ring->out_head;
        s_ring->out_buf[head % OUT_RING_SIZE] = (uint8_t)*p;
        __asm__ volatile("" ::: "memory");
        s_ring->out_head = head + 1;
    }
}

/* Decimal formatter (no libc sprintf in freestanding PD). */
static void out_uint(uint32_t val) {
    char tmp[12]; int i = 0;
    if (val == 0) { out_write("0"); return; }
    while (val && i < 11) { tmp[i++] = '0' + (int)(val % 10); val /= 10; }
    char rev[12]; int k = 0;
    for (int j = i - 1; j >= 0; j--) rev[k++] = tmp[j];
    rev[k] = '\0';
    out_write(rev);
}

/* Hex byte formatter. */
static const char s_hex[] = "0123456789abcdef";
static void out_hex_byte(uint8_t b) {
    char tmp[3] = { s_hex[b >> 4], s_hex[b & 0xF], '\0' };
    out_write(tmp);
}

/* ── Known MR registry ────────────────────────────────────────────────────── */
/*
 * Declared weak so they default to 0 when not mapped into dev_shell PD.
 * cmd_mem_dump and cmd_perf_show guard against vaddr == 0 before dereferencing.
 */
uintptr_t perf_ring_vaddr     __attribute__((weak));
uintptr_t mem_ring_vaddr       __attribute__((weak));
uintptr_t vibe_code_vaddr      __attribute__((weak));
uintptr_t vibe_state_vaddr     __attribute__((weak));
uintptr_t gpu_tensor_buf_vaddr __attribute__((weak));

typedef struct { const char *name; uintptr_t *vaddr_ptr; size_t size; } MREntry;

static const MREntry s_known_mrs[] = {
    { "perf_ring",      &perf_ring_vaddr,      0x1000    },
    { "mem_ring",       &mem_ring_vaddr,        0x40000  },
    { "vibe_code",      &vibe_code_vaddr,       0x400000 },
    { "vibe_state",     &vibe_state_vaddr,      0x10000  },
    { "gpu_tensor_buf", &gpu_tensor_buf_vaddr,  0x4000000},
    { "dev_shell_ring", &dev_shell_ring_vaddr,  0x1000   },
};
#define KNOWN_MR_COUNT ((size_t)(sizeof(s_known_mrs) / sizeof(s_known_mrs[0])))

/* ── Command handlers ─────────────────────────────────────────────────────── */

static void cmd_help(void) {
    out_write("agentOS dev_shell commands:\r\n"
              "  help\r\n"
              "  version\r\n"
              "  echo <text>\r\n"
              "  pd list\r\n"
              "  pd stat <id>\r\n"
              "  mem dump <mr_name> <offset_hex> <len_dec>\r\n"
              "  ipc send <ch_id> <op_hex> <arg_hex>\r\n"
              "  trace dump\r\n"
              "  fault inject <slot_id> <kind:vm|cap|null>\r\n"
              "  perf show\r\n"
              "  mr list\r\n"
              "  quit\r\n");
}

static void cmd_version(void) {
    out_write("agentOS dev_shell v0.1 (" AGENTOS_VERSION_STR ")\r\n");
}

static void cmd_echo(const char *args) {
    out_write(args);
    out_write("\r\n");
}

static void cmd_pd_list(void) {
    out_write("PD list:\r\n"
              "  0  controller        prio=50\r\n"
              "  1  swap_slot_0       prio=75\r\n"
              "  2  swap_slot_1       prio=75\r\n"
              "  3  swap_slot_2       prio=75\r\n"
              "  4  swap_slot_3       prio=75\r\n"
              "  5  worker_0..7       prio=80\r\n"
              "  6  init_agent        prio=90\r\n"
              "  7  fault_inject      prio=85\r\n"
              "  8  mem_profiler      prio=108\r\n"
              "  9  event_bus         prio=200\r\n"
              " 10  perf_counters     prio=95\r\n"
              " 11  vibe_engine       prio=140\r\n"
              " 12  agentfs           prio=150\r\n"
              " 13  console_mux       prio=160\r\n"
              " 14  time_partition    prio=250\r\n"
              " 15  dev_shell         prio=70  (this PD)\r\n");
}

static void cmd_pd_stat(const char *args) {
    out_write("pd stat id=");
    out_write(args);
    out_write(" (PPC to controller not yet wired — TODO)\r\n");
}

static void cmd_mr_list(void) {
    out_write("Known MRs:\r\n");
    for (size_t i = 0; i < KNOWN_MR_COUNT; i++) {
        out_write("  "); out_write(s_known_mrs[i].name);
        uintptr_t v = *s_known_mrs[i].vaddr_ptr;
        out_write("  vaddr=0x");
        for (int s = 28; s >= 0; s -= 4) {
            char c = s_hex[(v >> s) & 0xF];
            out_write(&c);
        }
        if (!v) out_write("  (not mapped)");
        out_write("\r\n");
    }
}

static void cmd_mem_dump(const char *args) {
    char mr_name[64] = {0};
    unsigned long offset = 0, len = 0;
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) mr_name[i++] = *p++;
    while (*p == ' ') p++;
    if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) p += 2;
    while (*p && *p != ' ') {
        offset = offset * 16 + (unsigned long)(*p >= 'a' ? *p - 'a' + 10
                                              : *p >= 'A' ? *p - 'A' + 10
                                              : *p - '0');
        p++;
    }
    while (*p == ' ') p++;
    while (*p && *p != ' ') { len = len * 10 + (unsigned long)(*p - '0'); p++; }

    const MREntry *mr = NULL;
    for (size_t k = 0; k < KNOWN_MR_COUNT; k++) {
        if (strcmp(s_known_mrs[k].name, mr_name) == 0) { mr = &s_known_mrs[k]; break; }
    }
    if (!mr)             { out_write("unknown MR: "); out_write(mr_name); out_write("\r\n"); return; }
    if (!*mr->vaddr_ptr) { out_write("MR not mapped in dev_shell\r\n"); return; }
    if (len == 0 || len > 256) len = 64;
    if (offset + len > mr->size) { out_write("offset+len > MR size\r\n"); return; }

    const uint8_t *base = (const uint8_t *)(*mr->vaddr_ptr + offset);
    for (unsigned long row = 0; row < len; row += 16) {
        out_write("  ");
        out_hex_byte((uint8_t)((offset + row) >> 8));
        out_hex_byte((uint8_t)(offset + row));
        out_write(": ");
        for (unsigned long col = 0; col < 16 && row + col < len; col++) {
            out_hex_byte(base[row + col]); out_write(" ");
        }
        out_write("\r\n");
    }
}

static void cmd_ipc_send(const char *args) {
    unsigned long ch = 0, op = 0, arg = 0;
    const char *p = args;
    while (*p && *p != ' ') { ch = ch * 10 + (unsigned long)(*p - '0'); p++; }
    while (*p == ' ') p++;
    if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) p += 2;
    while (*p && *p != ' ') {
        op = op * 16 + (unsigned long)(*p >= 'a' ? *p - 'a' + 10
                                      : *p >= 'A' ? *p - 'A' + 10
                                      : *p - '0');
        p++;
    }
    while (*p == ' ') p++;
    if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) p += 2;
    while (*p) {
        arg = arg * 16 + (unsigned long)(*p >= 'a' ? *p - 'a' + 10
                                        : *p >= 'A' ? *p - 'A' + 10
                                        : *p - '0');
        p++;
    }
    if (ch != DEV_SHELL_CTRL_CH) {
        out_write("ipc send: only ch=");
        out_uint((uint32_t)DEV_SHELL_CTRL_CH);
        out_write(" (controller) allowed\r\n");
        return;
    }
    microkit_mr_set(0, (uint64_t)op);
    microkit_mr_set(1, (uint64_t)arg);
    microkit_notify((microkit_channel)ch);
    out_write("ipc send: notified ch="); out_uint((uint32_t)ch);
    out_write(" op=0x"); out_hex_byte((uint8_t)op);
    out_write(" arg=0x"); out_hex_byte((uint8_t)arg);
    out_write("\r\n");
}

static void cmd_trace_dump(void) {
    microkit_mr_set(0, OP_TRACE_DUMP);
    microkit_notify(DEV_SHELL_TRACE_CH);
    out_write("trace dump: notified trace_recorder\r\n");
}

static void cmd_fault_inject(const char *args) {
    unsigned long slot = 0;
    uint8_t kind = 0;
    const char *p = args;
    while (*p && *p != ' ') { slot = slot * 10 + (unsigned long)(*p - '0'); p++; }
    while (*p == ' ') p++;
    if      (strncmp(p, "vm",   2) == 0 || strncmp(p, "null", 4) == 0) kind = FAULT_NULL_DEREF;
    else if (strncmp(p, "cap",  3) == 0)                                kind = FAULT_STACK_OVF;
    else {
        if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) p += 2;
        while (*p) {
            kind = (uint8_t)(kind * 16 + (unsigned char)(*p >= 'a' ? *p - 'a' + 10
                                                         : *p >= 'A' ? *p - 'A' + 10
                                                         : *p - '0'));
            p++;
        }
    }
    microkit_mr_set(0, OP_FAULT_INJECT);
    microkit_mr_set(1, (uint64_t)slot);
    microkit_mr_set(2, (uint64_t)kind);
    microkit_notify(DEV_SHELL_FAULT_CH);
    out_write("fault inject: slot="); out_uint((uint32_t)slot);
    out_write(" kind=0x"); out_hex_byte(kind);
    out_write(" — notified fault_inject PD\r\n");
}

static void cmd_perf_show(void) {
    out_write("perf show: reading perf_ring...\r\n");
    if (!perf_ring_vaddr) { out_write("  perf_ring not mapped in dev_shell\r\n"); return; }
    typedef struct { uint64_t ts_ns; uint8_t pd_id; uint8_t ch; uint64_t lat; uint8_t _p[6]; } PRec;
    const PRec *ring = (const PRec *)perf_ring_vaddr;
    for (int j = 0; j < 4; j++) {
        out_write("  ["); out_uint((uint32_t)j); out_write("] pd=");
        out_uint(ring[j].pd_id); out_write(" ch=");
        out_uint(ring[j].ch);    out_write(" lat=");
        out_uint((uint32_t)ring[j].lat); out_write("ns\r\n");
    }
}

/* ── Command dispatcher ───────────────────────────────────────────────────── */

static void dispatch(const char *line) {
    if (!line || !*line) return;
    while (*line == ' ') line++;

    if      (strcmp(line, "help") == 0)               cmd_help();
    else if (strcmp(line, "version") == 0)             cmd_version();
    else if (strncmp(line, "echo ", 5) == 0)           cmd_echo(line + 5);
    else if (strcmp(line, "pd list") == 0)             cmd_pd_list();
    else if (strncmp(line, "pd stat ", 8) == 0)        cmd_pd_stat(line + 8);
    else if (strcmp(line, "mr list") == 0)             cmd_mr_list();
    else if (strncmp(line, "mem dump ", 9) == 0)       cmd_mem_dump(line + 9);
    else if (strncmp(line, "ipc send ", 9) == 0)       cmd_ipc_send(line + 9);
    else if (strcmp(line, "trace dump") == 0)          cmd_trace_dump();
    else if (strncmp(line, "fault inject ", 13) == 0)  cmd_fault_inject(line + 13);
    else if (strcmp(line, "perf show") == 0)           cmd_perf_show();
    else if (strcmp(line, "quit") == 0)
        out_write("quit: shell stays active\r\n");
    else {
        out_write("unknown command: "); out_write(line);
        out_write("\r\nType 'help' for command list.\r\n");
    }
}

/* ── Microkit entry points ────────────────────────────────────────────────── */

void init(void) {
    ring_init();
    microkit_dbg_puts("[dev_shell] PD online (priority 70, AGENTOS_DEV_SHELL)\n");
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

microkit_msginfo_t protected(microkit_channel ch, microkit_msginfo_t msginfo) {
    /* dev_shell is not a PPC target */
    (void)ch; (void)msginfo;
    microkit_mr_set(0, 0xDEAD);
    return microkit_msginfo_new(0, 1);
}

#else /* !AGENTOS_DEV_SHELL */

/* ── Stub: no-op when build flag is absent ────────────────────────────────── */

uintptr_t dev_shell_ring_vaddr;

void init(void) {
    microkit_dbg_puts("[dev_shell] disabled (build with -DAGENTOS_DEV_SHELL)\n");
}

void notified(microkit_channel ch) { (void)ch; }

microkit_msginfo_t protected(microkit_channel ch, microkit_msginfo_t msginfo) {
    (void)ch; (void)msginfo;
    microkit_mr_set(0, 0xDEAD);
    return microkit_msginfo_new(0, 1);
}

#endif /* AGENTOS_DEV_SHELL */
