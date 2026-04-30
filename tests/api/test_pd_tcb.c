/*
 * test_pd_tcb.c — API tests for the pd_tcb protection-domain thread lifecycle
 *
 * Covered entry points:
 *   pd_tcb_create    — allocate TCB, bind CSpace/VSpace/IPC buffer, set priority
 *   pd_tcb_set_regs  — write PC, SP, and first argument register
 *   pd_tcb_start     — resume (make runnable)
 *   pd_tcb_suspend   — suspend a running thread
 *
 * The tests run entirely on the host.  All seL4 invocations are provided by
 * the stub layer in this file, which records call arguments in static
 * variables and returns a configurable error code.  The ut_alloc, seL4 type,
 * and constant definitions come from the real production headers so the test
 * exercises exactly the same struct layout as the target build.
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST \
 *      -I tests/api \
 *      -I kernel/agentos-root-task/include \
 *      -std=c11 -Wall -Wextra \
 *      -o /tmp/t_pd_tcb \
 *      tests/api/test_pd_tcb.c && /tmp/t_pd_tcb
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST
#include "framework.h"

/*
 * Pull in the production type definitions (seL4_Word, seL4_CPtr, seL4_Error,
 * seL4_UserContext, seL4_TCBObject, seL4_CapNull, etc.) and the function
 * *declarations* from sel4_boot.h and ut_alloc.h.
 *
 * We then provide our own *definitions* (stub bodies) for those functions
 * below, before including pd_tcb.c.
 */
#include "sel4_boot.h"
#include "ut_alloc.h"

/* ── Stub call-record structures ─────────────────────────────────────────── */

typedef struct {
    uint32_t   type;
    uint32_t   size_bits;
    seL4_CPtr  dest_cnode;
    seL4_Word  dest_index;
    uint32_t   dest_depth;
    uint32_t   call_count;
} StubUtAlloc;

typedef struct {
    seL4_CPtr  service;
    seL4_CPtr  cspace_root;
    seL4_Word  cspace_root_data;
    seL4_CPtr  vspace_root;
    seL4_Word  vspace_root_data;
    seL4_Word  buffer;
    seL4_CPtr  buffer_frame;
    uint32_t   call_count;
} StubTCBConfigure;

typedef struct {
    seL4_CPtr service;
    seL4_CPtr authority;
    seL4_Word priority;
    uint32_t  call_count;
} StubTCBSetPriority;

typedef struct {
    seL4_CPtr          service;
    uint8_t            resume;
    uint8_t            arch_flags;
    seL4_Word          count;
    seL4_UserContext   regs;
    uint32_t           call_count;
} StubTCBWriteRegisters;

typedef struct {
    seL4_CPtr service;
    uint32_t  call_count;
} StubTCBResume;

typedef struct {
    seL4_CPtr service;
    uint32_t  call_count;
} StubTCBSuspend;

/* ── Stub state ──────────────────────────────────────────────────────────── */

static StubUtAlloc            g_ut;
static StubTCBConfigure       g_cfg;
static StubTCBSetPriority     g_prio;
static StubTCBWriteRegisters  g_wrregs;
static StubTCBResume          g_resume;
static StubTCBSuspend         g_suspend;

/* Injected error codes — default seL4_NoError */
static seL4_Error g_ut_err;
static seL4_Error g_cfg_err;
static seL4_Error g_prio_err;
static seL4_Error g_wrregs_err;
static seL4_Error g_resume_err;
static seL4_Error g_suspend_err;

static void stub_reset(void) {
    memset(&g_ut,      0, sizeof(g_ut));
    memset(&g_cfg,     0, sizeof(g_cfg));
    memset(&g_prio,    0, sizeof(g_prio));
    memset(&g_wrregs,  0, sizeof(g_wrregs));
    memset(&g_resume,  0, sizeof(g_resume));
    memset(&g_suspend, 0, sizeof(g_suspend));
    g_ut_err      = seL4_NoError;
    g_cfg_err     = seL4_NoError;
    g_prio_err    = seL4_NoError;
    g_wrregs_err  = seL4_NoError;
    g_resume_err  = seL4_NoError;
    g_suspend_err = seL4_NoError;
}

/* ── Stub implementations ─────────────────────────────────────────────────── */

seL4_Error ut_alloc(uint32_t type, uint32_t size_bits,
                    seL4_CPtr dest_cnode, seL4_Word dest_index,
                    uint32_t dest_depth) {
    g_ut.type       = type;
    g_ut.size_bits  = size_bits;
    g_ut.dest_cnode = dest_cnode;
    g_ut.dest_index = dest_index;
    g_ut.dest_depth = dest_depth;
    g_ut.call_count++;
    return g_ut_err;
}

seL4_Error seL4_TCB_Configure(seL4_CPtr service,
                               seL4_CPtr cspace_root,
                               seL4_Word cspace_root_data,
                               seL4_CPtr vspace_root,
                               seL4_Word vspace_root_data,
                               seL4_Word buffer,
                               seL4_CPtr buffer_frame) {
    g_cfg.service          = service;
    g_cfg.cspace_root      = cspace_root;
    g_cfg.cspace_root_data = cspace_root_data;
    g_cfg.vspace_root      = vspace_root;
    g_cfg.vspace_root_data = vspace_root_data;
    g_cfg.buffer           = buffer;
    g_cfg.buffer_frame     = buffer_frame;
    g_cfg.call_count++;
    return g_cfg_err;
}

seL4_Error seL4_TCB_SetPriority(seL4_CPtr service,
                                 seL4_CPtr authority,
                                 seL4_Word priority) {
    g_prio.service   = service;
    g_prio.authority = authority;
    g_prio.priority  = priority;
    g_prio.call_count++;
    return g_prio_err;
}

seL4_Error seL4_TCB_WriteRegisters(seL4_CPtr         service,
                                    uint8_t           resume,
                                    uint8_t           arch_flags,
                                    seL4_Word         count,
                                    seL4_UserContext *regs) {
    g_wrregs.service    = service;
    g_wrregs.resume     = resume;
    g_wrregs.arch_flags = arch_flags;
    g_wrregs.count      = count;
    if (regs) g_wrregs.regs = *regs;
    g_wrregs.call_count++;
    return g_wrregs_err;
}

seL4_Error seL4_TCB_Resume(seL4_CPtr service) {
    g_resume.service = service;
    g_resume.call_count++;
    return g_resume_err;
}

seL4_Error seL4_TCB_Suspend(seL4_CPtr service) {
    g_suspend.service = service;
    g_suspend.call_count++;
    return g_suspend_err;
}

/* ── Include the implementation under test ──────────────────────────────── */
/*
 * pd_tcb.c includes pd_tcb.h -> sel4_boot.h and ut_alloc.h.  Both headers
 * are guarded by #pragma once so the re-include is a no-op.  The seL4_*
 * and ut_alloc function bodies above satisfy the linker.
 */
#include "../../kernel/agentos-root-task/src/pd_tcb.c"

/* ── Test helpers ─────────────────────────────────────────────────────────── */

/* Canonical test parameters */
#define TEST_DEST_CNODE   seL4_CapInitThreadCNode
#define TEST_DEST_SLOT    ((seL4_Word)42u)
#define TEST_VSPACE_CAP   ((seL4_CPtr)10u)
#define TEST_PD_CNODE     ((seL4_CPtr)11u)
#define TEST_IPC_BUF_CAP  ((seL4_CPtr)12u)
#define TEST_IPC_BUF_VA   ((seL4_Word)0x8000u)
#define TEST_PRIORITY     ((uint8_t)128u)
#define TEST_CNODE_SIZE_BITS ((uint8_t)6u)

#define TEST_ENTRY        ((seL4_Word)0x400000u)
#define TEST_SP           ((seL4_Word)0x7fff0000u)
#define TEST_ARG0         ((seL4_Word)0xCAFEu)
#define TEST_ARG1         ((seL4_Word)0xBEEFu)

static pd_tcb_result_t do_create(void) {
    return pd_tcb_create(TEST_DEST_CNODE, TEST_DEST_SLOT,
                         TEST_VSPACE_CAP, TEST_PD_CNODE,
                         TEST_IPC_BUF_CAP, TEST_IPC_BUF_VA,
                         TEST_PRIORITY, TEST_CNODE_SIZE_BITS);
}

/* ════════════════════════════════════════════════════════════════════════════
 * pd_tcb_create tests
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_create_success(void) {
    stub_reset();
    pd_tcb_result_t r = do_create();
    ASSERT_EQ((uint64_t)r.error,   (uint64_t)seL4_NoError, "create: error == seL4_NoError on success");
    ASSERT_EQ((uint64_t)r.tcb_cap, (uint64_t)TEST_DEST_SLOT, "create: tcb_cap == dest_slot on success");
}

static void test_create_calls_ut_alloc(void) {
    stub_reset();
    do_create();
    ASSERT_EQ((uint64_t)g_ut.call_count, 1u,
              "create: ut_alloc called exactly once");
    ASSERT_EQ((uint64_t)g_ut.type, (uint64_t)seL4_TCBObject,
              "create: ut_alloc requested seL4_TCBObject");
    ASSERT_EQ((uint64_t)g_ut.dest_cnode, (uint64_t)TEST_DEST_CNODE,
              "create: ut_alloc dest_cnode == passed dest_cnode");
    ASSERT_EQ((uint64_t)g_ut.dest_index, (uint64_t)TEST_DEST_SLOT,
              "create: ut_alloc dest_index == passed dest_slot");
}

static void test_create_calls_tcb_configure(void) {
    stub_reset();
    do_create();
    ASSERT_EQ((uint64_t)g_cfg.call_count, 1u,
              "create: seL4_TCB_Configure called exactly once");
    ASSERT_EQ((uint64_t)g_cfg.service,     (uint64_t)TEST_DEST_SLOT,   "create: configure service == dest_slot");
    ASSERT_EQ((uint64_t)g_cfg.vspace_root, (uint64_t)TEST_VSPACE_CAP,  "create: configure vspace_root == vspace_cap");
    ASSERT_EQ((uint64_t)g_cfg.cspace_root, (uint64_t)TEST_PD_CNODE,    "create: configure cspace_root == pd_cnode");
    ASSERT_EQ((uint64_t)g_cfg.cspace_root_data,
              (uint64_t)(seL4_WordBits - TEST_CNODE_SIZE_BITS),
              "create: configure cspace_root_data encodes guard size");
    ASSERT_EQ((uint64_t)g_cfg.buffer,      (uint64_t)TEST_IPC_BUF_VA,  "create: configure buffer == ipc_buf_va");
    ASSERT_EQ((uint64_t)g_cfg.buffer_frame,(uint64_t)TEST_IPC_BUF_CAP, "create: configure bufferFrame == ipc_buf_cap");
    ASSERT_EQ((uint64_t)g_cfg.vspace_root_data, 0u, "create: configure vspace_root_data == 0");
}

static void test_create_calls_set_priority(void) {
    stub_reset();
    do_create();
    ASSERT_EQ((uint64_t)g_prio.call_count, 1u,
              "create: seL4_TCB_SetPriority called exactly once");
    ASSERT_EQ((uint64_t)g_prio.service,   (uint64_t)TEST_DEST_SLOT,
              "create: SetPriority service == dest_slot");
    ASSERT_EQ((uint64_t)g_prio.authority, (uint64_t)seL4_CapInitThreadTCB,
              "create: SetPriority authority == seL4_CapInitThreadTCB");
    ASSERT_EQ((uint64_t)g_prio.priority,  (uint64_t)TEST_PRIORITY,
              "create: SetPriority priority == passed priority");
}

static void test_create_ut_alloc_failure(void) {
    stub_reset();
    g_ut_err = seL4_NotEnoughMemory;
    pd_tcb_result_t r = do_create();
    ASSERT_NE((uint64_t)r.error, (uint64_t)seL4_NoError,
              "create: error != seL4_NoError when ut_alloc fails");
    ASSERT_EQ((uint64_t)r.tcb_cap, (uint64_t)seL4_CapNull,
              "create: tcb_cap == seL4_CapNull when ut_alloc fails");
    /* TCB_Configure must NOT be called if ut_alloc failed */
    ASSERT_EQ((uint64_t)g_cfg.call_count, 0u,
              "create: seL4_TCB_Configure not called after ut_alloc failure");
}

static void test_create_configure_failure(void) {
    stub_reset();
    g_cfg_err = seL4_InvalidCapability;
    pd_tcb_result_t r = do_create();
    ASSERT_NE((uint64_t)r.error, (uint64_t)seL4_NoError,
              "create: error != seL4_NoError when TCB_Configure fails");
    ASSERT_EQ((uint64_t)r.tcb_cap, (uint64_t)seL4_CapNull,
              "create: tcb_cap == seL4_CapNull when TCB_Configure fails");
    /* SetPriority must NOT be called if Configure failed */
    ASSERT_EQ((uint64_t)g_prio.call_count, 0u,
              "create: SetPriority not called after TCB_Configure failure");
}

static void test_create_set_priority_failure(void) {
    stub_reset();
    g_prio_err = seL4_InvalidArgument;
    pd_tcb_result_t r = do_create();
    ASSERT_NE((uint64_t)r.error, (uint64_t)seL4_NoError,
              "create: error != seL4_NoError when SetPriority fails");
    ASSERT_EQ((uint64_t)r.tcb_cap, (uint64_t)seL4_CapNull,
              "create: tcb_cap == seL4_CapNull when SetPriority fails");
}

static void test_create_priority_zero(void) {
    stub_reset();
    pd_tcb_result_t r = pd_tcb_create(TEST_DEST_CNODE, TEST_DEST_SLOT,
                                       TEST_VSPACE_CAP, TEST_PD_CNODE,
                                       TEST_IPC_BUF_CAP, TEST_IPC_BUF_VA,
                                       0 /* lowest priority */,
                                       TEST_CNODE_SIZE_BITS);
    ASSERT_EQ((uint64_t)r.error, (uint64_t)seL4_NoError,
              "create: priority 0 (lowest) is accepted");
    ASSERT_EQ((uint64_t)g_prio.priority, 0u,
              "create: priority 0 passed verbatim to SetPriority");
}

static void test_create_priority_max(void) {
    stub_reset();
    pd_tcb_result_t r = pd_tcb_create(TEST_DEST_CNODE, TEST_DEST_SLOT,
                                       TEST_VSPACE_CAP, TEST_PD_CNODE,
                                       TEST_IPC_BUF_CAP, TEST_IPC_BUF_VA,
                                       255 /* highest priority */,
                                       TEST_CNODE_SIZE_BITS);
    ASSERT_EQ((uint64_t)r.error, (uint64_t)seL4_NoError,
              "create: priority 255 (highest) is accepted");
    ASSERT_EQ((uint64_t)g_prio.priority, 255u,
              "create: priority 255 passed verbatim to SetPriority");
}

/* ════════════════════════════════════════════════════════════════════════════
 * pd_tcb_set_regs tests
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_set_regs_success(void) {
    stub_reset();
    seL4_Error err = pd_tcb_set_regs(TEST_DEST_SLOT,
                                      TEST_ENTRY, TEST_SP, TEST_ARG0, TEST_ARG1);
    ASSERT_EQ((uint64_t)err, (uint64_t)seL4_NoError,
              "set_regs: returns seL4_NoError on success");
}

static void test_set_regs_calls_write_registers(void) {
    stub_reset();
    pd_tcb_set_regs(TEST_DEST_SLOT, TEST_ENTRY, TEST_SP, TEST_ARG0, TEST_ARG1);
    ASSERT_EQ((uint64_t)g_wrregs.call_count, 1u,
              "set_regs: seL4_TCB_WriteRegisters called exactly once");
    ASSERT_EQ((uint64_t)g_wrregs.service, (uint64_t)TEST_DEST_SLOT,
              "set_regs: WriteRegisters service == tcb_cap");
}

static void test_set_regs_resume_flag(void) {
    stub_reset();
    pd_tcb_set_regs(TEST_DEST_SLOT, TEST_ENTRY, TEST_SP, TEST_ARG0, TEST_ARG1);
    ASSERT_EQ((uint64_t)g_wrregs.resume, 1u,
              "set_regs: resume == 1 (thread is enqueued atomically)");
}

static void test_set_regs_pc_set(void) {
    stub_reset();
    pd_tcb_set_regs(TEST_DEST_SLOT, TEST_ENTRY, TEST_SP, TEST_ARG0, TEST_ARG1);
    ASSERT_EQ((uint64_t)g_wrregs.regs.pc, (uint64_t)TEST_ENTRY,
              "set_regs: PC == entry point");
}

static void test_set_regs_sp_set(void) {
    stub_reset();
    pd_tcb_set_regs(TEST_DEST_SLOT, TEST_ENTRY, TEST_SP, TEST_ARG0, TEST_ARG1);
    ASSERT_EQ((uint64_t)g_wrregs.regs.sp, (uint64_t)TEST_SP,
              "set_regs: SP == stack_top");
}

static void test_set_regs_arg0_set(void) {
    stub_reset();
    pd_tcb_set_regs(TEST_DEST_SLOT, TEST_ENTRY, TEST_SP, TEST_ARG0, TEST_ARG1);
    ASSERT_EQ((uint64_t)g_wrregs.regs.x0, (uint64_t)TEST_ARG0,
              "set_regs: x0 == arg0");
}

static void test_set_regs_other_regs_zero(void) {
    stub_reset();
    pd_tcb_set_regs(TEST_DEST_SLOT, TEST_ENTRY, TEST_SP, TEST_ARG0, TEST_ARG1);
    ASSERT_EQ((uint64_t)g_wrregs.regs.x1, (uint64_t)TEST_ARG1,
              "set_regs: x1 == arg1");
    ASSERT_EQ((uint64_t)g_wrregs.regs.x2,  0u, "set_regs: x2 == 0");
    ASSERT_EQ((uint64_t)g_wrregs.regs.x30, 0u, "set_regs: link register x30 == 0");
}

static void test_set_regs_full_context_written(void) {
    stub_reset();
    pd_tcb_set_regs(TEST_DEST_SLOT, TEST_ENTRY, TEST_SP, TEST_ARG0, TEST_ARG1);
    ASSERT_EQ((uint64_t)g_wrregs.count, (uint64_t)seL4_UserContext_n_regs,
              "set_regs: count == seL4_UserContext_n_regs (all regs written)");
}

static void test_set_regs_write_failure(void) {
    stub_reset();
    g_wrregs_err = seL4_InvalidCapability;
    seL4_Error err = pd_tcb_set_regs(TEST_DEST_SLOT,
                                      TEST_ENTRY, TEST_SP, TEST_ARG0, TEST_ARG1);
    ASSERT_NE((uint64_t)err, (uint64_t)seL4_NoError,
              "set_regs: propagates error from WriteRegisters");
}

static void test_set_regs_zero_entry(void) {
    stub_reset();
    seL4_Error err = pd_tcb_set_regs(TEST_DEST_SLOT, 0, TEST_SP, TEST_ARG0, TEST_ARG1);
    ASSERT_EQ((uint64_t)err, (uint64_t)seL4_NoError,
              "set_regs: entry == 0 is accepted (stub does not reject it)");
    ASSERT_EQ((uint64_t)g_wrregs.regs.pc, 0u,
              "set_regs: PC == 0 when entry is 0");
}

/* ════════════════════════════════════════════════════════════════════════════
 * pd_tcb_start tests
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_start_success(void) {
    stub_reset();
    seL4_Error err = pd_tcb_start(TEST_DEST_SLOT);
    ASSERT_EQ((uint64_t)err, (uint64_t)seL4_NoError,
              "start: returns seL4_NoError on success");
}

static void test_start_calls_resume(void) {
    stub_reset();
    pd_tcb_start(TEST_DEST_SLOT);
    ASSERT_EQ((uint64_t)g_resume.call_count, 1u,
              "start: seL4_TCB_Resume called exactly once");
    ASSERT_EQ((uint64_t)g_resume.service, (uint64_t)TEST_DEST_SLOT,
              "start: Resume invoked on the correct TCB cap");
}

static void test_start_failure(void) {
    stub_reset();
    g_resume_err = seL4_InvalidCapability;
    seL4_Error err = pd_tcb_start(TEST_DEST_SLOT);
    ASSERT_NE((uint64_t)err, (uint64_t)seL4_NoError,
              "start: propagates error from seL4_TCB_Resume");
}

/* ════════════════════════════════════════════════════════════════════════════
 * pd_tcb_suspend tests
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_suspend_success(void) {
    stub_reset();
    seL4_Error err = pd_tcb_suspend(TEST_DEST_SLOT);
    ASSERT_EQ((uint64_t)err, (uint64_t)seL4_NoError,
              "suspend: returns seL4_NoError on success");
}

static void test_suspend_calls_suspend(void) {
    stub_reset();
    pd_tcb_suspend(TEST_DEST_SLOT);
    ASSERT_EQ((uint64_t)g_suspend.call_count, 1u,
              "suspend: seL4_TCB_Suspend called exactly once");
    ASSERT_EQ((uint64_t)g_suspend.service, (uint64_t)TEST_DEST_SLOT,
              "suspend: Suspend invoked on the correct TCB cap");
}

static void test_suspend_failure(void) {
    stub_reset();
    g_suspend_err = seL4_InvalidCapability;
    seL4_Error err = pd_tcb_suspend(TEST_DEST_SLOT);
    ASSERT_NE((uint64_t)err, (uint64_t)seL4_NoError,
              "suspend: propagates error from seL4_TCB_Suspend");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Full lifecycle integration test
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_full_lifecycle(void) {
    stub_reset();

    /* Step 1: create */
    pd_tcb_result_t r = do_create();
    ASSERT_EQ((uint64_t)r.error,   (uint64_t)seL4_NoError,    "lifecycle: create succeeds");
    ASSERT_NE((uint64_t)r.tcb_cap, (uint64_t)seL4_CapNull,    "lifecycle: tcb_cap is non-null");

    /* Step 2: write registers */
    seL4_Error err = pd_tcb_set_regs(r.tcb_cap, TEST_ENTRY, TEST_SP, TEST_ARG0, TEST_ARG1);
    ASSERT_EQ((uint64_t)err, (uint64_t)seL4_NoError, "lifecycle: set_regs succeeds");

    /* Step 3: start */
    err = pd_tcb_start(r.tcb_cap);
    ASSERT_EQ((uint64_t)err, (uint64_t)seL4_NoError, "lifecycle: start succeeds");

    /* Step 4: suspend */
    err = pd_tcb_suspend(r.tcb_cap);
    ASSERT_EQ((uint64_t)err, (uint64_t)seL4_NoError, "lifecycle: suspend succeeds");

    /* Step 5: resume again */
    err = pd_tcb_start(r.tcb_cap);
    ASSERT_EQ((uint64_t)err, (uint64_t)seL4_NoError, "lifecycle: second start succeeds");

    /* Verify invocation ordering: ut_alloc → configure → set_priority
     *                             → write_registers → resume → suspend → resume */
    ASSERT_EQ((uint64_t)g_ut.call_count,      1u, "lifecycle: ut_alloc called once");
    ASSERT_EQ((uint64_t)g_cfg.call_count,     1u, "lifecycle: TCB_Configure called once");
    ASSERT_EQ((uint64_t)g_prio.call_count,    1u, "lifecycle: SetPriority called once");
    ASSERT_EQ((uint64_t)g_wrregs.call_count,  1u, "lifecycle: WriteRegisters called once");
    ASSERT_EQ((uint64_t)g_resume.call_count,  2u, "lifecycle: Resume called twice (start+restart)");
    ASSERT_EQ((uint64_t)g_suspend.call_count, 1u, "lifecycle: Suspend called once");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    /*
     * Total test points:
     *   test_create_success             :  2
     *   test_create_calls_ut_alloc      :  4
     *   test_create_calls_tcb_configure :  8
     *   test_create_calls_set_priority  :  4
     *   test_create_ut_alloc_failure    :  3
     *   test_create_configure_failure   :  3
     *   test_create_set_priority_failure:  2
     *   test_create_priority_zero       :  2
     *   test_create_priority_max        :  2
     *   create subtotal                 : 30
     *
     *   test_set_regs_success           :  1
     *   test_set_regs_calls_write_regs  :  2
     *   test_set_regs_resume_flag       :  1
     *   test_set_regs_pc_set            :  1
     *   test_set_regs_sp_set            :  1
     *   test_set_regs_arg0_set          :  1
     *   test_set_regs_other_regs_zero   :  3
     *   test_set_regs_full_context      :  1
     *   test_set_regs_write_failure     :  1
     *   test_set_regs_zero_entry        :  2
     *   set_regs subtotal               : 14
     *
     *   test_start_success              :  1
     *   test_start_calls_resume         :  2
     *   test_start_failure              :  1
     *   start subtotal                  :  4
     *
     *   test_suspend_success            :  1
     *   test_suspend_calls_suspend      :  2
     *   test_suspend_failure            :  1
     *   suspend subtotal                :  4
     *
     *   test_full_lifecycle             : 12
     *
     *   TOTAL = 30 + 14 + 4 + 4 + 12 = 64
     */
    TAP_PLAN(64);

    /* pd_tcb_create */
    test_create_success();
    test_create_calls_ut_alloc();
    test_create_calls_tcb_configure();
    test_create_calls_set_priority();
    test_create_ut_alloc_failure();
    test_create_configure_failure();
    test_create_set_priority_failure();
    test_create_priority_zero();
    test_create_priority_max();

    /* pd_tcb_set_regs */
    test_set_regs_success();
    test_set_regs_calls_write_registers();
    test_set_regs_resume_flag();
    test_set_regs_pc_set();
    test_set_regs_sp_set();
    test_set_regs_arg0_set();
    test_set_regs_other_regs_zero();
    test_set_regs_full_context_written();
    test_set_regs_write_failure();
    test_set_regs_zero_entry();

    /* pd_tcb_start */
    test_start_success();
    test_start_calls_resume();
    test_start_failure();

    /* pd_tcb_suspend */
    test_suspend_success();
    test_suspend_calls_suspend();
    test_suspend_failure();

    /* Full lifecycle */
    test_full_lifecycle();

    return tap_exit();
}

#else
typedef int _agentos_api_test_pd_tcb_dummy;
#endif /* AGENTOS_TEST_HOST */
