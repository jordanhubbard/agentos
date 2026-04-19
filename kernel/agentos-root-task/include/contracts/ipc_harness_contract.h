#pragma once
/* IPC_HARNESS contract — version 1
 * PD: ipc_harness (test-only PD) | Source: tests/ipc_harness.c
 * NOTE: This PD is instantiated only in simulator/test builds. It is excluded from production images.
 */
#include <stdint.h>
#include <stdbool.h>

#define IPC_HARNESS_CONTRACT_VERSION 1

/* ── Opcodes ── */
#define IPC_HARNESS_OP_RUN         0xF200u  /* run a named test case */
#define IPC_HARNESS_OP_STATUS      0xF201u  /* query test runner state */
#define IPC_HARNESS_OP_RESULT      0xF202u  /* retrieve result of last completed test */
#define IPC_HARNESS_OP_RESET       0xF203u  /* reset test state machine */
#define IPC_HARNESS_OP_LIST        0xF204u  /* list available test cases into shmem */

/* ── Test result codes ── */
#define IPC_HARNESS_RESULT_PASS    0u  /* test passed */
#define IPC_HARNESS_RESULT_FAIL    1u  /* test failed (assertion) */
#define IPC_HARNESS_RESULT_ERROR   2u  /* test error (setup/teardown failure) */
#define IPC_HARNESS_RESULT_SKIP    3u  /* test skipped (precondition not met) */
#define IPC_HARNESS_RESULT_TIMEOUT 4u  /* test exceeded time limit */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* IPC_HARNESS_OP_RUN */
    uint32_t test_id;         /* numeric test identifier */
    uint32_t timeout_ms;      /* maximum execution time (0 = default 5000ms) */
    uint32_t flags;           /* IPC_HARNESS_FLAG_* */
} ipc_harness_req_run_t;

#define IPC_HARNESS_FLAG_VERBOSE   (1u << 0)  /* write detailed log to shmem */
#define IPC_HARNESS_FLAG_NO_SETUP  (1u << 1)  /* skip test setup (for idempotency checks) */

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = test accepted, else ipc_harness_error_t */
    uint32_t run_id;          /* unique run identifier */
} ipc_harness_reply_run_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* IPC_HARNESS_OP_STATUS */
} ipc_harness_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t running;         /* 1 if a test is currently executing */
    uint32_t current_test_id; /* test_id of running test (0 if idle) */
    uint32_t tests_run;       /* total tests executed since boot */
    uint32_t tests_passed;
    uint32_t tests_failed;
    uint32_t tests_skipped;
} ipc_harness_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* IPC_HARNESS_OP_RESULT */
    uint32_t run_id;          /* run_id from RUN reply */
    uint32_t shmem_offset;    /* byte offset for verbose log output */
    uint32_t max_log_bytes;
} ipc_harness_req_result_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, IPC_HARNESS_ERR_PENDING if still running */
    uint32_t result;          /* IPC_HARNESS_RESULT_* */
    uint32_t test_id;         /* echoed */
    uint32_t duration_ms;     /* actual execution time */
    uint32_t log_bytes;       /* bytes written to shmem (0 if not verbose) */
    uint32_t assertion_line;  /* source line of failing assertion (0 = pass or no info) */
} ipc_harness_reply_result_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* IPC_HARNESS_OP_RESET */
} ipc_harness_req_reset_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} ipc_harness_reply_reset_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* IPC_HARNESS_OP_LIST */
    uint32_t shmem_offset;
    uint32_t max_entries;
} ipc_harness_req_list_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t entry_count;
    uint32_t total_tests;
} ipc_harness_reply_list_t;

/* Test case descriptor written to shmem during LIST */
typedef struct __attribute__((packed)) {
    uint32_t test_id;
    uint8_t  name[48];        /* null-terminated test name */
    uint32_t flags;           /* IPC_HARNESS_FLAG_* that apply to this test */
} ipc_harness_test_entry_t;

/* ── Error codes ── */
typedef enum {
    IPC_HARNESS_OK             = 0,
    IPC_HARNESS_ERR_BUSY       = 1,  /* RUN called while another test is running */
    IPC_HARNESS_ERR_NOT_FOUND  = 2,  /* test_id does not exist */
    IPC_HARNESS_ERR_PENDING    = 3,  /* RESULT called before test completed */
    IPC_HARNESS_ERR_NO_SHMEM   = 4,  /* verbose output or list region not mapped */
} ipc_harness_error_t;

/* ── Invariants ──
 * - ipc_harness is excluded from production images; it is present only in simulator builds.
 * - Only one test may run at a time; RUN returns IPC_HARNESS_ERR_BUSY if occupied.
 * - run_id is monotonically increasing per session; RESET restarts it from 1.
 * - RESULT is non-blocking: if the test is still running, IPC_HARNESS_ERR_PENDING is returned.
 * - Test output in shmem follows TAP (Test Anything Protocol) format when verbose.
 * - RESET aborts any running test and returns the harness to IDLE state.
 */
