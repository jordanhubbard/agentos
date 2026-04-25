/*
 * test_proc_server.c — Host-side unit tests for proc_server
 *
 * Exercises the process table directly: spawn, exit, wait, status,
 * list, kill, and setcap operations.
 *
 * Build:
 *   cc -DAGENTOS_TEST_HOST -I kernel/agentos-root-task/include \
 *      tests/test_proc_server.c -o /tmp/test_proc_server
 * Run:
 *   /tmp/test_proc_server
 */

#ifdef AGENTOS_TEST_HOST

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* ── Microkit stubs ──────────────────────────────────────────────────────── */
typedef uint32_t microkit_channel;
typedef uint64_t microkit_msginfo;

static uint64_t _mrs[16];
static inline uint64_t microkit_mr_get(int i)           { return _mrs[i]; }
static inline void     microkit_mr_set(int i, uint64_t v) { _mrs[i] = v; }
static inline microkit_msginfo microkit_msginfo_new(uint64_t l, uint64_t c) {
    return (l << 12) | c;
}
static inline void microkit_dbg_puts(const char *s) { (void)s; }

/* proc_shmem_vaddr — declared here; proc_server.c references it as extern-less
 * global when AGENTOS_TEST_HOST is defined */
static uintptr_t proc_shmem_vaddr;

/* ── seL4 IPC stubs (proc_server.c uses these when AGENTOS_TEST_HOST) ────── */
#ifndef SEL4_BADGE_DEFINED
typedef uint64_t sel4_badge_t;
#define SEL4_BADGE_DEFINED 1
#endif
#ifndef SEL4_MSG_DATA_BYTES
#define SEL4_MSG_DATA_BYTES 48u
#endif
#ifndef SEL4_MSG_T_DEFINED
typedef struct { uint32_t opcode; uint32_t length; uint8_t data[SEL4_MSG_DATA_BYTES]; } sel4_msg_t;
#define SEL4_MSG_T_DEFINED 1
#endif
#define SEL4_ERR_OK 0u
static inline uint32_t msg_u32(const sel4_msg_t *m, uint32_t off) {
    uint32_t v = 0;
    if (off + 4u <= SEL4_MSG_DATA_BYTES) __builtin_memcpy(&v, m->data + off, 4);
    return v;
}
static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u <= SEL4_MSG_DATA_BYTES) { __builtin_memcpy(m->data + off, &v, 4); m->length = off + 4u; }
}
static inline void sel4_dbg_puts(const char *s) { (void)s; }

/* ── proc_server opcodes (must match agentos.h) ──────────────────────────── */
#define OP_PROC_SPAWN   0xD0u
#define OP_PROC_EXIT    0xD1u
#define OP_PROC_WAIT    0xD2u
#define OP_PROC_STATUS  0xD3u
#define OP_PROC_LIST    0xD4u
#define OP_PROC_KILL    0xD5u
#define OP_PROC_SETCAP  0xD6u

/* ── Pull in the implementation ─────────────────────────────────────────── */
#include "../kernel/agentos-root-task/src/proc_server.c"

/* ── Microkit→seL4 IPC shims ─────────────────────────────────────────────── *
 * proc_server.c was migrated from Microkit to raw seL4 IPC; the test still
 * drives it via the old _mrs[] pattern.  These shims pack/unpack the message
 * registers into sel4_msg_t so the dispatch function can be called uniformly.
 * Must appear after proc_server.c include since the functions are static.
 */
static void init(void) { proc_server_pd_init(); }

static void protected(uint32_t ch, uint64_t msginfo)
{
    (void)ch; (void)msginfo;
    sel4_msg_t req = {0}, rep = {0};
    /* msg_u32(req, 0) is the opcode; pack _mrs[i] → req.data[i*4] */
    for (unsigned i = 0; i < 8 && i * 4u + 4u <= SEL4_MSG_DATA_BYTES; i++) {
        uint32_t v = (uint32_t)_mrs[i];
        __builtin_memcpy(req.data + i * 4, &v, 4);
    }
    req.length = 8u * 4u;
    proc_server_pd_dispatch(0, &req, &rep, NULL);
    for (unsigned i = 0; i < 8 && i * 4u + 4u <= SEL4_MSG_DATA_BYTES; i++) {
        _mrs[i] = msg_u32(&rep, i * 4u);
    }
}

/* ── Test harness ────────────────────────────────────────────────────────── */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("=== TEST: %s ===\n", #name)
#define ASSERT_EQ(a, b) do { \
    if ((uint64_t)(a) == (uint64_t)(b)) { \
        printf("  PASS: " #a " == " #b "\n"); \
        tests_passed++; \
    } else { \
        printf("  FAIL: " #a "=%llu != " #b "=%llu (line %d)\n", \
               (unsigned long long)(uint64_t)(a), \
               (unsigned long long)(uint64_t)(b), \
               __LINE__); \
        tests_failed++; \
    } \
} while (0)

int main(void)
{
    proc_shmem_vaddr = 0;
    init();

    /* ── Test: pid=0 "kernel" process exists and is RUNNING ─────────────── */
    TEST(kernel_proc_exists);
    _mrs[0] = OP_PROC_STATUS; _mrs[1] = 0;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0u);                  /* ok */
    ASSERT_EQ(_mrs[1], PROC_STATE_RUNNING);  /* state */
    ASSERT_EQ(_mrs[2], 0xFFu);              /* full caps */

    /* ── Test: lookup nonexistent pid → error ────────────────────────────── */
    TEST(status_nonexistent);
    _mrs[0] = OP_PROC_STATUS; _mrs[1] = 9999;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0xFFu);

    /* ── Test: spawn a process ───────────────────────────────────────────── */
    TEST(spawn_process);
    _mrs[0] = OP_PROC_SPAWN; _mrs[1] = 0; _mrs[2] = 1; _mrs[3] = 0x3Fu;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0u);   /* ok */
    uint32_t pid1 = (uint32_t)_mrs[1];
    ASSERT_EQ(pid1, 1u);      /* first allocated pid */

    /* ── Test: status of newly spawned process ───────────────────────────── */
    TEST(spawn_status_running);
    _mrs[0] = OP_PROC_STATUS; _mrs[1] = pid1;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0u);
    ASSERT_EQ(_mrs[1], PROC_STATE_RUNNING);
    ASSERT_EQ(_mrs[2], 0x3Fu);  /* cap_mask preserved */

    /* ── Test: process list shows 2 entries (kernel + pid1) ─────────────── */
    TEST(proc_list_count);
    uint8_t list_buf[512];
    for (int i = 0; i < 512; i++) list_buf[i] = 0;
    proc_shmem_vaddr = (uintptr_t)list_buf;
    _mrs[0] = OP_PROC_LIST;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0u);
    ASSERT_EQ(_mrs[1], 2u);  /* kernel + pid1 */
    proc_shmem_vaddr = 0;

    /* ── Test: exit a process → ZOMBIE ──────────────────────────────────── */
    TEST(proc_exit);
    _mrs[0] = OP_PROC_EXIT; _mrs[1] = pid1; _mrs[2] = 42;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0u);
    _mrs[0] = OP_PROC_STATUS; _mrs[1] = pid1;
    protected(0, 0);
    ASSERT_EQ(_mrs[1], PROC_STATE_ZOMBIE);

    /* ── Test: wait on zombie → immediate reply with exit code ──────────── */
    TEST(proc_wait_zombie);
    _mrs[0] = OP_PROC_WAIT; _mrs[1] = pid1;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0u);
    ASSERT_EQ(_mrs[1], 42u);               /* exit_code */
    ASSERT_EQ(_mrs[2], PROC_STATE_ZOMBIE); /* state */

    /* ── Test: spawn a second process ───────────────────────────────────── */
    TEST(spawn_second);
    _mrs[0] = OP_PROC_SPAWN; _mrs[1] = 0; _mrs[2] = 2; _mrs[3] = 0x0Fu;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0u);
    uint32_t pid2 = (uint32_t)_mrs[1];
    ASSERT_EQ(pid2, 2u);

    /* ── Test: kill with SIGKILL (9) → ZOMBIE ───────────────────────────── */
    TEST(kill_sigkill);
    _mrs[0] = OP_PROC_KILL; _mrs[1] = pid2; _mrs[2] = 9;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0u);
    _mrs[0] = OP_PROC_STATUS; _mrs[1] = pid2;
    protected(0, 0);
    ASSERT_EQ(_mrs[1], PROC_STATE_ZOMBIE);
    ASSERT_EQ(_mrs[0], 0u);

    /* ── Test: spawn a third process, then SIGSTOP → STOPPED ────────────── */
    TEST(kill_sigstop);
    _mrs[0] = OP_PROC_SPAWN; _mrs[1] = 0; _mrs[2] = 3; _mrs[3] = 0x01u;
    protected(0, 0);
    uint32_t pid3 = (uint32_t)_mrs[1];
    _mrs[0] = OP_PROC_KILL; _mrs[1] = pid3; _mrs[2] = 19;  /* SIGSTOP */
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0u);
    _mrs[0] = OP_PROC_STATUS; _mrs[1] = pid3;
    protected(0, 0);
    ASSERT_EQ(_mrs[1], PROC_STATE_STOPPED);

    /* ── Test: SIGCONT restores STOPPED → RUNNING ───────────────────────── */
    TEST(kill_sigcont);
    _mrs[0] = OP_PROC_KILL; _mrs[1] = pid3; _mrs[2] = 18;  /* SIGCONT */
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0u);
    _mrs[0] = OP_PROC_STATUS; _mrs[1] = pid3;
    protected(0, 0);
    ASSERT_EQ(_mrs[1], PROC_STATE_RUNNING);

    /* ── Test: kill nonexistent pid → error ─────────────────────────────── */
    TEST(kill_nonexistent);
    _mrs[0] = OP_PROC_KILL; _mrs[1] = 9999; _mrs[2] = 9;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0xFFu);

    /* ── Test: setcap updates cap_mask ──────────────────────────────────── */
    TEST(setcap);
    _mrs[0] = OP_PROC_SETCAP; _mrs[1] = pid3; _mrs[2] = 0xABu;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0u);
    _mrs[0] = OP_PROC_STATUS; _mrs[1] = pid3;
    protected(0, 0);
    ASSERT_EQ(_mrs[2], 0xABu);  /* cap_mask updated */

    /* ── Test: setcap on nonexistent pid → error ─────────────────────────── */
    TEST(setcap_nonexistent);
    _mrs[0] = OP_PROC_SETCAP; _mrs[1] = 9999; _mrs[2] = 0xFFu;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0xFFu);

    /* ── Test: wait on nonexistent pid → error ──────────────────────────── */
    TEST(wait_nonexistent);
    _mrs[0] = OP_PROC_WAIT; _mrs[1] = 9999;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0xFFu);

    /* ── Test: unknown opcode → error ───────────────────────────────────── */
    TEST(unknown_op);
    _mrs[0] = 0xFFu;
    protected(0, 0);
    ASSERT_EQ(_mrs[0], 0xFFu);

    printf("\n=== RESULTS: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}

#endif /* AGENTOS_TEST_HOST */
