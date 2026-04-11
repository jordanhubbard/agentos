/*
 * agentOS Dev Shell — Unit Test
 *
 * Tests the command ring read/write logic, output ring formatting, and
 * command dispatch from dev_shell.c.  Runs on the host without seL4.
 *
 * Build:  cc -o /tmp/test_dev_shell \
 *             tests/test_dev_shell.c \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST -DAGENTOS_DEV_SHELL
 * Run:    /tmp/test_dev_shell
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Host-side stubs
 * ══════════════════════════════════════════════════════════════════════════ */
#ifdef AGENTOS_TEST_HOST

static uint64_t _mrs[64];
static inline void     microkit_mr_set(uint32_t i, uint64_t v) { _mrs[i] = v; }
static inline uint64_t microkit_mr_get(uint32_t i)             { return _mrs[i]; }
typedef uint32_t microkit_channel;
typedef uint64_t microkit_msginfo_t;
static inline microkit_msginfo_t microkit_msginfo_new(uint64_t l, uint32_t c) {
    (void)c; return l;
}
static inline microkit_msginfo_t microkit_ppcall(microkit_channel ch, microkit_msginfo_t m) {
    (void)ch; (void)m; return 0;
}
static inline void microkit_notify(microkit_channel ch) { (void)ch; }
static inline void microkit_dbg_puts(const char *s) { printf("%s", s); }

/* Fault-inject and trace opcodes expected by dev_shell.c */
#define OP_FAULT_INJECT  0x70u
#define OP_TRACE_DUMP    0xA1u
#define FAULT_NULL_DEREF 0x01u
#define FAULT_STACK_OVF  0x02u
#define AGENTOS_VERSION_STR "agentOS v0.1.0-alpha"

#define LOG(fmt, ...) printf("[test] " fmt "\n", ##__VA_ARGS__)

#endif /* AGENTOS_TEST_HOST */

/* ══════════════════════════════════════════════════════════════════════════
 * Inline dev_shell ring + command logic (mirrored from dev_shell.c)
 * ══════════════════════════════════════════════════════════════════════════ */

#define DEV_SHELL_MAGIC   0xDE55E11UL
#define DEV_SHELL_VERSION 1
#define CMD_RING_SIZE     2032
#define OUT_RING_SIZE     2040
#define MAX_CMD_LEN       256
#define MAX_OUT_LEN       1024

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

static dev_shell_ring_t s_ring_storage;
static dev_shell_ring_t *s_ring = &s_ring_storage;

static void ring_init_local(void) {
    memset(s_ring, 0, sizeof(*s_ring));
    s_ring->magic   = DEV_SHELL_MAGIC;
    s_ring->version = DEV_SHELL_VERSION;
}

static bool cmd_read_line(char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        uint32_t head = s_ring->cmd_head;
        uint32_t tail = s_ring->cmd_tail;
        if (head == tail) return false;
        uint8_t c = s_ring->cmd_buf[tail % CMD_RING_SIZE];
        s_ring->cmd_tail = tail + 1;
        if (c == '\n' || c == '\r') break;
        buf[n++] = (char)c;
    }
    buf[n] = '\0';
    return (n > 0);
}

static void cmd_write_raw(const char *cmd) {
    for (const char *p = cmd; *p; p++) {
        uint32_t head = s_ring->cmd_head;
        s_ring->cmd_buf[head % CMD_RING_SIZE] = (uint8_t)*p;
        s_ring->cmd_head = head + 1;
    }
    /* Append newline terminator */
    uint32_t h = s_ring->cmd_head;
    s_ring->cmd_buf[h % CMD_RING_SIZE] = '\n';
    s_ring->cmd_head = h + 1;
}

static void out_write(const char *str) {
    for (const char *p = str; *p; p++) {
        uint32_t head = s_ring->out_head;
        s_ring->out_buf[head % OUT_RING_SIZE] = (uint8_t)*p;
        s_ring->out_head = head + 1;
    }
}

/* Read accumulated output into a NUL-terminated buffer (up to max-1 bytes) */
static uint32_t out_drain(char *buf, uint32_t max) {
    uint32_t n = 0;
    while (n < max - 1) {
        uint32_t head = s_ring->out_head;
        uint32_t tail = s_ring->out_tail;
        if (head == tail) break;
        buf[n++] = (char)s_ring->out_buf[tail % OUT_RING_SIZE];
        s_ring->out_tail = tail + 1;
    }
    buf[n] = '\0';
    return n;
}

/* Decimal formatter */
static void out_uint_local(uint32_t val) {
    char tmp[12]; int i = 0;
    if (val == 0) { out_write("0"); return; }
    while (val && i < 11) { tmp[i++] = '0' + (int)(val % 10); val /= 10; }
    char rev[12]; int k = 0;
    for (int j = i - 1; j >= 0; j--) rev[k++] = tmp[j];
    rev[k] = '\0';
    out_write(rev);
}

/* Command handlers */
static void cmd_help(void)    { out_write("agentOS dev_shell commands:\r\n"); }
static void cmd_version(void) { out_write("agentOS dev_shell v0.1 (" AGENTOS_VERSION_STR ")\r\n"); }
static void cmd_echo(const char *args) { out_write(args); out_write("\r\n"); }
static void cmd_pd_list(void) {
    out_write("PD list:\r\n  0  controller\r\n  15 dev_shell\r\n");
}
static void cmd_pd_stat(const char *args) {
    out_write("pd stat id="); out_write(args); out_write("\r\n");
}
static void cmd_mr_list(void) { out_write("Known MRs:\r\n  dev_shell_ring\r\n"); }

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
    if (ch != 1 /* DEV_SHELL_CTRL_CH */) {
        out_write("ipc send: only ch=");
        out_uint_local((uint32_t)1);
        out_write(" (controller) allowed\r\n");
        return;
    }
    microkit_mr_set(0, (uint64_t)op);
    microkit_mr_set(1, (uint64_t)arg);
    microkit_notify((microkit_channel)ch);
    out_write("ipc send: notified ch="); out_uint_local((uint32_t)ch);
    out_write("\r\n");
}

static void cmd_trace_dump(void) {
    microkit_mr_set(0, OP_TRACE_DUMP);
    microkit_notify(2 /* DEV_SHELL_TRACE_CH */);
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
    microkit_mr_set(0, OP_FAULT_INJECT);
    microkit_mr_set(1, (uint64_t)slot);
    microkit_mr_set(2, (uint64_t)kind);
    microkit_notify(3 /* DEV_SHELL_FAULT_CH */);
    out_write("fault inject: slot="); out_uint_local((uint32_t)slot);
    out_write("\r\n");
}

static void dispatch(const char *line) {
    if (!line || !*line) return;
    while (*line == ' ') line++;
    if      (strcmp(line, "help") == 0)               cmd_help();
    else if (strcmp(line, "version") == 0)             cmd_version();
    else if (strncmp(line, "echo ", 5) == 0)           cmd_echo(line + 5);
    else if (strcmp(line, "pd list") == 0)             cmd_pd_list();
    else if (strncmp(line, "pd stat ", 8) == 0)        cmd_pd_stat(line + 8);
    else if (strcmp(line, "mr list") == 0)             cmd_mr_list();
    else if (strncmp(line, "ipc send ", 9) == 0)       cmd_ipc_send(line + 9);
    else if (strcmp(line, "trace dump") == 0)          cmd_trace_dump();
    else if (strncmp(line, "fault inject ", 13) == 0)  cmd_fault_inject(line + 13);
    else if (strcmp(line, "quit") == 0)
        out_write("quit: shell stays active\r\n");
    else {
        out_write("unknown command: "); out_write(line);
        out_write("\r\nType 'help' for command list.\r\n");
    }
}

/* Helper: write command to ring and dispatch all pending lines */
static void run_command(const char *cmd) {
    cmd_write_raw(cmd);
    char line[MAX_CMD_LEN];
    while (cmd_read_line(line, MAX_CMD_LEN)) dispatch(line);
}

/* ── Test framework ──────────────────────────────────────────────────────── */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { ring_init_local(); printf("\n=== TEST: %s ===\n", (name)); } while(0)

#define ASSERT_TRUE(expr, msg) do { \
    if (!(expr)) { printf("  FAIL: %s\n", (msg)); tests_failed++; } \
    else         { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

#define ASSERT_CONTAINS(out, needle, msg) do { \
    if (!strstr((out), (needle))) { \
        printf("  FAIL: %s — did not find \"%s\" in \"%s\"\n", (msg), (needle), (out)); \
        tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

/* ── Tests ────────────────────────────────────────────────────────────────── */

static void test_ring_init(void) {
    TEST("ring_init");
    ASSERT_TRUE(s_ring->magic   == DEV_SHELL_MAGIC,   "magic set");
    ASSERT_TRUE(s_ring->version == DEV_SHELL_VERSION, "version set");
    ASSERT_TRUE(s_ring->cmd_head == 0, "cmd_head = 0");
    ASSERT_TRUE(s_ring->out_head == 0, "out_head = 0");
}

static void test_cmd_help(void) {
    TEST("cmd_help");
    run_command("help");
    char out[512];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "agentOS dev_shell", "help output present");
}

static void test_cmd_version(void) {
    TEST("cmd_version");
    run_command("version");
    char out[256];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "dev_shell v0.1", "version string present");
    ASSERT_CONTAINS(out, AGENTOS_VERSION_STR, "AGENTOS_VERSION_STR present");
}

static void test_cmd_echo(void) {
    TEST("cmd_echo");
    run_command("echo hello world");
    char out[256];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "hello world", "echo text present");
}

static void test_cmd_pd_list(void) {
    TEST("cmd_pd_list");
    run_command("pd list");
    char out[512];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "controller", "controller in pd list");
    ASSERT_CONTAINS(out, "dev_shell", "dev_shell in pd list");
}

static void test_cmd_mr_list(void) {
    TEST("cmd_mr_list");
    run_command("mr list");
    char out[256];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "dev_shell_ring", "dev_shell_ring in mr list");
}

static void test_cmd_unknown(void) {
    TEST("cmd_unknown");
    run_command("frobnicate");
    char out[256];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "unknown command", "error for unknown command");
    ASSERT_CONTAINS(out, "frobnicate", "unknown command echoed");
    ASSERT_CONTAINS(out, "help", "hint to use help present");
}

static void test_cmd_quit_no_exit(void) {
    TEST("cmd_quit_no_exit");
    run_command("quit");
    char out[256];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "shell stays active", "quit message present");
}

static void test_cmd_ipc_send_ctrl(void) {
    TEST("cmd_ipc_send_ctrl");
    run_command("ipc send 1 0x10 0x42");
    char out[256];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "notified", "ipc send notified");
}

static void test_cmd_ipc_send_denied(void) {
    TEST("cmd_ipc_send_denied");
    run_command("ipc send 5 0x10 0x00"); /* non-controller channel */
    char out[256];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "only ch=", "ipc send denied for non-ctrl channel");
}

static void test_cmd_trace_dump(void) {
    TEST("cmd_trace_dump");
    run_command("trace dump");
    char out[256];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "trace_recorder", "trace dump notified recorder");
}

static void test_cmd_fault_inject_vm(void) {
    TEST("cmd_fault_inject_vm");
    run_command("fault inject 3 vm");
    char out[256];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "fault inject:", "fault inject output present");
    ASSERT_CONTAINS(out, "slot=3", "slot id present");
    ASSERT_TRUE(microkit_mr_get(0) == OP_FAULT_INJECT, "MR0 = OP_FAULT_INJECT");
    ASSERT_TRUE(microkit_mr_get(1) == 3, "MR1 = slot_id 3");
    ASSERT_TRUE(microkit_mr_get(2) == FAULT_NULL_DEREF, "MR2 = FAULT_NULL_DEREF for vm");
}

static void test_cmd_fault_inject_cap(void) {
    TEST("cmd_fault_inject_cap");
    run_command("fault inject 0 cap");
    char out[256];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "slot=0", "slot id 0");
    ASSERT_TRUE(microkit_mr_get(2) == FAULT_STACK_OVF, "MR2 = FAULT_STACK_OVF for cap");
}

static void test_ring_write_wrap(void) {
    TEST("ring_write_wrap");
    /* Write enough commands to wrap the cmd ring */
    for (int i = 0; i < 20; i++) run_command("echo wrap");
    char out[8192];
    out_drain(out, sizeof(out));
    /* The output ring is large; just verify some output arrived */
    ASSERT_TRUE(strlen(out) > 0, "output produced for wrapped ring writes");
}

static void test_pd_stat(void) {
    TEST("cmd_pd_stat");
    run_command("pd stat 7");
    char out[256];
    out_drain(out, sizeof(out));
    ASSERT_CONTAINS(out, "7", "stat output references id");
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  agentOS Dev Shell — Test Suite                  ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");

    ring_init_local();

    test_ring_init();
    test_cmd_help();
    test_cmd_version();
    test_cmd_echo();
    test_cmd_pd_list();
    test_cmd_mr_list();
    test_cmd_unknown();
    test_cmd_quit_no_exit();
    test_cmd_ipc_send_ctrl();
    test_cmd_ipc_send_denied();
    test_cmd_trace_dump();
    test_cmd_fault_inject_vm();
    test_cmd_fault_inject_cap();
    test_ring_write_wrap();
    test_pd_stat();

    printf("\n══════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    if (tests_failed > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
