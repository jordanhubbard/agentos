#pragma once
/* DEBUG_BRIDGE contract — version 1
 * PD: debug_bridge | Source: src/debug_bridge.c | Channel: (privileged; controller-only access)
 */
#include <stdint.h>
#include <stdbool.h>

#define DEBUG_BRIDGE_CONTRACT_VERSION 1

/* ── Opcodes ── */
#define DEBUG_BRIDGE_OP_ATTACH        0xD0u  /* attach debugger to a PD */
#define DEBUG_BRIDGE_OP_DETACH        0xD1u  /* detach debugger from PD */
#define DEBUG_BRIDGE_OP_BREAKPOINT    0xD2u  /* set or clear a breakpoint */
#define DEBUG_BRIDGE_OP_STEP          0xD3u  /* single-step vCPU or PD thread */
#define DEBUG_BRIDGE_OP_READ_MEM      0xD4u  /* read guest or PD memory */
#define DEBUG_BRIDGE_OP_WRITE_MEM     0xD5u  /* write guest or PD memory */
#define DEBUG_BRIDGE_OP_READ_REGS     0xD6u  /* read PD/vCPU register set */
#define DEBUG_BRIDGE_OP_STATUS        0xD7u  /* query debug session state */

/* ── Breakpoint types ── */
#define DEBUG_BP_SW                   0u  /* software breakpoint (INT3 / BRK) */
#define DEBUG_BP_HW_EXEC              1u  /* hardware execution watchpoint */
#define DEBUG_BP_HW_READ              2u  /* hardware read watchpoint */
#define DEBUG_BP_HW_WRITE             3u  /* hardware write watchpoint */
#define DEBUG_BP_HW_RW                4u  /* hardware read/write watchpoint */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* DEBUG_BRIDGE_OP_ATTACH */
    uint32_t target_pd_id;    /* TRACE_PD_* or slot_id of target */
    uint32_t flags;           /* DEBUG_ATTACH_FLAG_* */
} debug_bridge_req_attach_t;

#define DEBUG_ATTACH_FLAG_HALT_ON_ATTACH  (1u << 0)  /* halt target immediately */
#define DEBUG_ATTACH_FLAG_VM_GUEST        (1u << 1)  /* target is inside a VM */

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else debug_bridge_error_t */
    uint32_t session_id;      /* opaque debug session handle */
} debug_bridge_reply_attach_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* DEBUG_BRIDGE_OP_DETACH */
    uint32_t session_id;
} debug_bridge_req_detach_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} debug_bridge_reply_detach_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* DEBUG_BRIDGE_OP_BREAKPOINT */
    uint32_t session_id;
    uint64_t addr;            /* virtual address for breakpoint */
    uint32_t bp_type;         /* DEBUG_BP_* */
    uint32_t set;             /* 1 = set, 0 = clear */
} debug_bridge_req_breakpoint_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t bp_id;           /* breakpoint handle (for clear) */
} debug_bridge_reply_breakpoint_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* DEBUG_BRIDGE_OP_STEP */
    uint32_t session_id;
    uint32_t step_count;      /* number of instructions to step (1 = single) */
} debug_bridge_req_step_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint64_t pc;              /* program counter after step */
} debug_bridge_reply_step_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* DEBUG_BRIDGE_OP_READ_MEM */
    uint32_t session_id;
    uint64_t vaddr;           /* virtual address to read from */
    uint32_t size;            /* bytes to read (max 4096) */
    uint32_t shmem_offset;    /* offset in shared region for output */
} debug_bridge_req_read_mem_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t bytes_read;
} debug_bridge_reply_read_mem_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* DEBUG_BRIDGE_OP_STATUS */
    uint32_t session_id;
} debug_bridge_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t target_pd_id;
    uint32_t halted;          /* 1 if target is halted at breakpoint or after step */
    uint32_t breakpoint_count;
    uint64_t halt_pc;         /* PC if halted */
} debug_bridge_reply_status_t;

/* ── Error codes ── */
typedef enum {
    DEBUG_BRIDGE_OK               = 0,
    DEBUG_BRIDGE_ERR_NO_SESSION   = 1,  /* session_id invalid */
    DEBUG_BRIDGE_ERR_ALREADY_ATT  = 2,  /* target already has a debug session */
    DEBUG_BRIDGE_ERR_NO_CAP       = 3,  /* caller lacks debug privilege */
    DEBUG_BRIDGE_ERR_BAD_ADDR     = 4,  /* vaddr not mapped in target */
    DEBUG_BRIDGE_ERR_HW_LIMIT     = 5,  /* hardware watchpoint slots exhausted */
    DEBUG_BRIDGE_ERR_NOT_HALTED   = 6,  /* step/reg-read requires halted target */
} debug_bridge_error_t;

/* ── Invariants ──
 * - debug_bridge is accessible only to the controller (Ring 1); no agent may call it directly.
 * - At most one debug session may be attached to a given PD at a time.
 * - Hardware breakpoints are limited by the architecture (ARM: 6 BP + 4 WP).
 * - Memory reads of more than 4096 bytes must be split across multiple calls.
 * - DETACH automatically clears all breakpoints and resumes the target.
 */
