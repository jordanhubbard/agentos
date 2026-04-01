/*
 * test_dev_shell.c — unit tests for agentOS dev_shell ring buffer protocol
 *
 * Standalone host test that validates the shared-memory contract used by
 * the dev_shell PD.  Mirrors the circular ring buffer layout and dispatch
 * logic from dev_shell.c, then exercises the full command/response cycle.
 *
 * Build & run (no special dependencies):
 *   cc test/test_dev_shell.c -o /tmp/test_dev_shell && /tmp/test_dev_shell
 *
 * The test passes if all assertions succeed and exits 0.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>

/* ── Ring layout — must match dev_shell.c dev_shell_ring_t ───────────────── */

#define CMD_RING_SIZE   2032
#define OUT_RING_SIZE   2040
#define DEV_SHELL_MAGIC 0xDE55E11UL
#define MAX_CMD_LEN     256

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

/* Compile-time size check: must fit in 4KB MR */
typedef char _ring_size_check[sizeof(dev_shell_ring_t) <= 4096 ? 1 : -1];

/* ── Dispatch simulation (mirrors dev_shell.c logic) ─────────────────────── */

static dev_shell_ring_t *s_ring;

static void out_write(const char *str) {
    for (const char *p = str; *p; p++) {
        uint32_t head = s_ring->out_head;
        s_ring->out_buf[head % OUT_RING_SIZE] = (uint8_t)*p;
        s_ring->out_head = head + 1;
    }
}

static void out_uint(uint32_t val) {
    char tmp[12]; int i = 0;
    if (val == 0) { out_write("0"); return; }
    while (val && i < 11) { tmp[i++] = '0' + (int)(val % 10); val /= 10; }
    char rev[12]; int k = 0;
    for (int j = i - 1; j >= 0; j--) rev[k++] = tmp[j];
    rev[k] = '\0';
    out_write(rev);
}

static void dispatch(const char *line) {
    while (*line == ' ') line++;

    if (strcmp(line, "help") == 0) {
        out_write("agentOS dev_shell commands:\r\n"
                  "  help\r\n  version\r\n  echo <text>\r\n"
                  "  pd list\r\n  pd stat <id>\r\n"
                  "  mem dump <mr_name> <offset_hex> <len_dec>\r\n"
                  "  ipc send <ch_id> <op_hex> <arg_hex>\r\n"
                  "  trace dump\r\n  fault inject <slot_id> <kind:vm|cap|null>\r\n"
                  "  perf show\r\n  mr list\r\n  quit\r\n");
    } else if (strcmp(line, "version") == 0) {
        out_write("agentOS dev_shell v0.1 (agentOS v0.1.0-alpha)\r\n");
    } else if (strncmp(line, "echo ", 5) == 0) {
        out_write(line + 5); out_write("\r\n");
    } else if (strcmp(line, "pd list") == 0) {
        out_write("PD list:\r\n");
        out_write("  0  controller        prio=50\r\n");
        out_write("  7  fault_inject      prio=85\r\n");
        out_write(" 15  dev_shell         prio=70  (this PD)\r\n");
    } else if (strncmp(line, "pd stat ", 8) == 0) {
        out_write("pd stat id="); out_write(line + 8);
        out_write(" (PPC to controller not yet wired — TODO)\r\n");
    } else if (strcmp(line, "mr list") == 0) {
        out_write("Known MRs:\r\n  dev_shell_ring  vaddr=0x");
        for (int s = 28; s >= 0; s -= 4) {
            char c = "0123456789abcdef"[(0xC000000 >> s) & 0xF];
            char cs[2] = { c, '\0' }; out_write(cs);
        }
        out_write("\r\n");
    } else if (strcmp(line, "trace dump") == 0) {
        out_write("trace dump: notified trace_recorder\r\n");
    } else if (strcmp(line, "perf show") == 0) {
        out_write("perf show: reading perf_ring...\r\n");
        out_write("  perf_ring not mapped in dev_shell\r\n");
    } else if (strncmp(line, "fault inject ", 13) == 0) {
        const char *p = line + 13;
        unsigned long slot = 0;
        while (*p && *p != ' ') { slot = slot * 10 + (unsigned long)(*p - '0'); p++; }
        while (*p == ' ') p++;
        out_write("fault inject: slot="); out_uint((uint32_t)slot);
        out_write(" kind=");
        if (strncmp(p, "vm", 2) == 0 || strncmp(p, "null", 4) == 0)
            out_write("0x01");
        else if (strncmp(p, "cap", 3) == 0)
            out_write("0x02");
        else
            out_write("0x??");
        out_write(" — notified fault_inject PD\r\n");
    } else if (strcmp(line, "quit") == 0) {
        out_write("quit: shell stays active\r\n");
    } else if (*line == '\0') {
        /* empty — ignore */
    } else {
        out_write("unknown command: "); out_write(line);
        out_write("\r\nType 'help' for command list.\r\n");
    }
}

/* ── Ring helpers ─────────────────────────────────────────────────────────── */

/* Write a command to the cmd ring (simulates host/controller side). */
static void ring_write_cmd(const char *cmd) {
    for (const char *p = cmd; *p; p++) {
        s_ring->cmd_buf[s_ring->cmd_head % CMD_RING_SIZE] = (uint8_t)*p;
        s_ring->cmd_head++;
    }
    /* Append newline to terminate the line */
    s_ring->cmd_buf[s_ring->cmd_head % CMD_RING_SIZE] = '\n';
    s_ring->cmd_head++;
}

/* Simulate dev_shell notified(): drain cmd ring, dispatch each line. */
static void ring_process(void) {
    char line[MAX_CMD_LEN];
    int  n = 0;
    while (1) {
        if (s_ring->cmd_head == s_ring->cmd_tail) break;
        uint8_t c = s_ring->cmd_buf[s_ring->cmd_tail % CMD_RING_SIZE];
        s_ring->cmd_tail++;
        if (c == '\n' || c == '\r') {
            line[n] = '\0';
            if (n > 0) { dispatch(line); out_write("> "); }
            n = 0;
        } else if (n < MAX_CMD_LEN - 1) {
            line[n++] = (char)c;
        }
    }
}

/* Read all available output from out ring into a null-terminated buffer. */
static void ring_read_output(char *buf, size_t bufsz) {
    size_t i = 0;
    while (s_ring->out_head != s_ring->out_tail && i < bufsz - 1) {
        buf[i++] = (char)s_ring->out_buf[s_ring->out_tail % OUT_RING_SIZE];
        s_ring->out_tail++;
    }
    buf[i] = '\0';
}

/* ── Test harness ─────────────────────────────────────────────────────────── */

static uint8_t g_backing[4096];

static void test_init(void) {
    memset(g_backing, 0, sizeof(g_backing));
    s_ring = (dev_shell_ring_t *)g_backing;
    s_ring->magic   = DEV_SHELL_MAGIC;
    s_ring->version = 1;
}

static char g_output[8192];

/* Send a command and capture the response. */
static const char *send_cmd(const char *cmd) {
    /* Reset out pointers so we only capture this command's response */
    s_ring->out_tail = s_ring->out_head;
    ring_write_cmd(cmd);
    ring_process();
    ring_read_output(g_output, sizeof(g_output));
    return g_output;
}

static void assert_contains(const char *output, const char *substr) {
    if (!strstr(output, substr)) {
        fprintf(stderr, "\n[FAIL] output does not contain '%s'\nActual:\n%s\n",
                substr, output);
        exit(1);
    }
    printf("  [PASS] contains '%s'\n", substr);
}

/* ── Tests ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== agentOS dev_shell unit tests ===\n\n");

    /* ── Test 1: ring buffer initialization ──────────────────────────────── */
    printf("[test 1] ring buffer initialization\n");
    test_init();
    assert(s_ring->magic   == DEV_SHELL_MAGIC);
    assert(s_ring->version == 1);
    assert(s_ring->cmd_head == 0);
    assert(s_ring->cmd_tail == 0);
    assert(s_ring->out_head == 0);
    assert(s_ring->out_tail == 0);
    assert(sizeof(dev_shell_ring_t) <= 4096);
    printf("  [PASS] ring magic=0x%x version=%u\n", s_ring->magic, s_ring->version);
    printf("  [PASS] DevShellRing fits in 4KB (%zu bytes)\n\n",
           sizeof(dev_shell_ring_t));

    /* ── Test 2: 'help' command ───────────────────────────────────────────── */
    printf("[test 2] 'help' command\n");
    const char *out = send_cmd("help");
    assert_contains(out, "help");
    assert_contains(out, "version");
    assert_contains(out, "pd list");
    assert_contains(out, "trace dump");
    assert_contains(out, "quit");
    printf("\n");

    /* ── Test 3: 'version' command ────────────────────────────────────────── */
    printf("[test 3] 'version' command\n");
    out = send_cmd("version");
    assert_contains(out, "dev_shell");
    assert_contains(out, "v0.1");
    printf("\n");

    /* ── Test 4: 'pd list' command ────────────────────────────────────────── */
    printf("[test 4] 'pd list' command\n");
    out = send_cmd("pd list");
    assert_contains(out, "PD list");
    assert_contains(out, "controller");
    assert_contains(out, "dev_shell");
    assert_contains(out, "prio=");
    printf("\n");

    /* ── Test 5: 'echo' command ───────────────────────────────────────────── */
    printf("[test 5] 'echo' command\n");
    out = send_cmd("echo hello agentOS");
    assert_contains(out, "hello agentOS");
    printf("\n");

    /* ── Test 6: 'trace dump' ─────────────────────────────────────────────── */
    printf("[test 6] 'trace dump'\n");
    out = send_cmd("trace dump");
    assert_contains(out, "trace");
    printf("\n");

    /* ── Test 7: 'fault inject' ───────────────────────────────────────────── */
    printf("[test 7] 'fault inject 1 vm'\n");
    out = send_cmd("fault inject 1 vm");
    assert_contains(out, "fault inject");
    assert_contains(out, "slot=1");
    printf("\n");

    /* ── Test 8: 'quit' no-op ─────────────────────────────────────────────── */
    printf("[test 8] 'quit' no-op\n");
    out = send_cmd("quit");
    assert_contains(out, "quit");
    assert_contains(out, "active");
    printf("\n");

    /* ── Test 9: unknown command returns error ────────────────────────────── */
    printf("[test 9] unknown command\n");
    out = send_cmd("bogus_cmd");
    assert_contains(out, "unknown command");
    assert_contains(out, "help");
    printf("\n");

    /* ── Test 10: multiple commands in one ring drain ─────────────────────── */
    printf("[test 10] multiple commands in one drain\n");
    s_ring->out_tail = s_ring->out_head;  /* reset output */
    ring_write_cmd("version");
    ring_write_cmd("help");
    ring_process();
    ring_read_output(g_output, sizeof(g_output));
    assert_contains(g_output, "dev_shell");
    assert_contains(g_output, "help");
    printf("\n");

    printf("=== all 10 tests passed ===\n");
    return 0;
}
