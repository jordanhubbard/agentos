/*
 * test_vibeos.c — API tests for the agentOS VibeOS VM lifecycle service
 *
 * Covered opcodes:
 *   OP_VOS_CREATE   (0x50) — create and configure a new OS instance
 *   OP_VOS_DESTROY  (0x51) — destroy an OS instance and free resources
 *   OP_VOS_STATUS   (0x52) — query the runtime state of an instance
 *   OP_VOS_LIST     (0x53) — enumerate all active instances
 *   OP_VOS_ATTACH   (0x54) — attach a device / capability to an instance
 *   OP_VOS_DETACH   (0x55) — detach a device / capability from an instance
 *   unknown opcode           — must return AOS_ERR_UNIMPL
 *
 * TODO: replace inline mock with
 *       #include "../../contracts/vibeos/interface.h"
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST -I tests/api -o /tmp/t_vibeos \
 *       tests/api/test_vibeos.c && /tmp/t_vibeos
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST
#include "framework.h"

/* ── Opcodes ─────────────────────────────────────────────────────────────── */
/* TODO: replace with #include "../../contracts/vibeos/interface.h" */
#define OP_VOS_CREATE   0x50u
#define OP_VOS_DESTROY  0x51u
#define OP_VOS_STATUS   0x52u
#define OP_VOS_LIST     0x53u
#define OP_VOS_ATTACH     0x54u
#define OP_VOS_DETACH     0x55u
#define OP_VOS_CONFIGURE  0x56u

/* ── OS instance states ───────────────────────────────────────────────────── */
#define VOS_STATE_STOPPED   0u
#define VOS_STATE_RUNNING   1u
#define VOS_STATE_PAUSED    2u
#define VOS_STATE_FAULTED   3u

/* ── OS image types ───────────────────────────────────────────────────────── */
#define VOS_IMAGE_LINUX    0u
#define VOS_IMAGE_FREEBSD  1u
#define VOS_IMAGE_CUSTOM   0xFFu

/* ── Device kinds that can be attached ──────────────────────────────────── */
#define VOS_DEV_SERIAL     0u
#define VOS_DEV_BLOCK      1u
#define VOS_DEV_NET        2u

/* ── Mock VibeOS ──────────────────────────────────────────────────────────── */

#define MOCK_MAX_INSTANCES   16u
#define MOCK_MAX_DEVICES     8u    /* per instance */
#define MOCK_NAME_LEN        32u

typedef struct {
    uint32_t dev_kind;
    uint32_t dev_id;
    uint32_t attached;
} MockAttachment;

typedef struct {
    uint32_t     id;          /* 1-based opaque instance handle */
    uint32_t     state;
    uint32_t     image_type;
    char         name[MOCK_NAME_LEN];
    uint32_t     mem_mb;      /* requested memory in MiB */
    uint32_t     vcpus;
    uint32_t     cpu_budget_us;
    uint32_t     cpu_period_us;
    MockAttachment devs[MOCK_MAX_DEVICES];
    uint32_t     active;
} MockInstance;

static MockInstance g_instances[MOCK_MAX_INSTANCES];
static uint32_t     g_next_id = 1u;

static void vos_reset(void) {
    memset(g_instances, 0, sizeof(g_instances));
    g_next_id = 1u;
}

static int vos_find(uint32_t id) {
    for (uint32_t i = 0; i < MOCK_MAX_INSTANCES; i++) {
        if (g_instances[i].active && g_instances[i].id == id)
            return (int)i;
    }
    return -1;
}

static int vos_alloc(void) {
    for (uint32_t i = 0; i < MOCK_MAX_INSTANCES; i++) {
        if (!g_instances[i].active) return (int)i;
    }
    return -1;
}

/*
 * vos_dispatch — mock VibeOS IPC handler.
 *
 * MR layout:
 *   OP_VOS_CREATE:
 *     MR[0]=opcode, MR[1]=name_ptr, MR[2]=name_len,
 *     MR[3]=image_type, MR[4]=mem_mb, MR[5]=vcpus
 *     Reply: MR[0]=AOS_OK|err, MR[1]=instance_id
 *
 *   OP_VOS_DESTROY:
 *     MR[0]=opcode, MR[1]=instance_id
 *     Reply: MR[0]=AOS_OK|AOS_ERR_NOT_FOUND|AOS_ERR_BUSY
 *
 *   OP_VOS_STATUS:
 *     MR[0]=opcode, MR[1]=instance_id
 *     Reply: MR[0]=AOS_OK|AOS_ERR_NOT_FOUND, MR[1]=state, MR[2]=mem_mb,
 *            MR[3]=vcpus
 *
 *   OP_VOS_LIST:
 *     MR[0]=opcode
 *     Reply: MR[0]=AOS_OK, MR[1]=count of active instances
 *
 *   OP_VOS_ATTACH:
 *     MR[0]=opcode, MR[1]=instance_id, MR[2]=dev_kind, MR[3]=dev_id
 *     Reply: MR[0]=AOS_OK|AOS_ERR_NOT_FOUND|AOS_ERR_NOSPC|AOS_ERR_EXISTS
 *
 *   OP_VOS_DETACH:
 *     MR[0]=opcode, MR[1]=instance_id, MR[2]=dev_kind, MR[3]=dev_id
 *     Reply: MR[0]=AOS_OK|AOS_ERR_NOT_FOUND
 */
static void vos_dispatch(microkit_channel ch, microkit_msginfo info) {
    (void)ch; (void)info;
    uint64_t op = _mrs[0];

    switch (op) {

    /* ── CREATE ─────────────────────────────────────────────────────────── */
    case OP_VOS_CREATE: {
        const char *name = (const char *)(uintptr_t)_mrs[1];
        uint32_t    nlen = (uint32_t)_mrs[2];
        uint32_t    img  = (uint32_t)_mrs[3];
        uint32_t    mb   = (uint32_t)_mrs[4];
        uint32_t    cpu  = (uint32_t)_mrs[5];

        if (!name || nlen == 0 || nlen >= MOCK_NAME_LEN) {
            _mrs[0] = AOS_ERR_INVAL; break;
        }
        if (mb == 0 || cpu == 0) {
            _mrs[0] = AOS_ERR_INVAL; break;
        }
        int slot = vos_alloc();
        if (slot < 0) { _mrs[0] = AOS_ERR_NOSPC; break; }
        MockInstance *inst = &g_instances[slot];
        inst->id         = g_next_id++;
        inst->state      = VOS_STATE_STOPPED;
        inst->image_type = img;
        inst->mem_mb     = mb;
        inst->vcpus      = cpu;
        inst->active     = 1u;
        memcpy(inst->name, name, nlen);
        inst->name[nlen] = '\0';
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)inst->id;
        break;
    }

    /* ── DESTROY ────────────────────────────────────────────────────────── */
    case OP_VOS_DESTROY: {
        uint32_t id = (uint32_t)_mrs[1];
        int slot = vos_find(id);
        if (slot < 0) { _mrs[0] = AOS_ERR_NOT_FOUND; break; }
        /* Disallow destroying a running instance (must stop first) */
        if (g_instances[slot].state == VOS_STATE_RUNNING) {
            _mrs[0] = AOS_ERR_BUSY; break;
        }
        memset(&g_instances[slot], 0, sizeof(g_instances[slot]));
        _mrs[0] = AOS_OK;
        break;
    }

    /* ── STATUS ─────────────────────────────────────────────────────────── */
    case OP_VOS_STATUS: {
        uint32_t id = (uint32_t)_mrs[1];
        int slot = vos_find(id);
        if (slot < 0) { _mrs[0] = AOS_ERR_NOT_FOUND; break; }
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)g_instances[slot].state;
        _mrs[2] = (uint64_t)g_instances[slot].mem_mb;
        _mrs[3] = (uint64_t)g_instances[slot].vcpus;
        break;
    }

    /* ── LIST ───────────────────────────────────────────────────────────── */
    case OP_VOS_LIST: {
        uint32_t count = 0u;
        for (uint32_t i = 0; i < MOCK_MAX_INSTANCES; i++) {
            if (g_instances[i].active) count++;
        }
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)count;
        break;
    }

    /* ── ATTACH ─────────────────────────────────────────────────────────── */
    case OP_VOS_ATTACH: {
        uint32_t id       = (uint32_t)_mrs[1];
        uint32_t dev_kind = (uint32_t)_mrs[2];
        uint32_t dev_id   = (uint32_t)_mrs[3];
        int slot = vos_find(id);
        if (slot < 0) { _mrs[0] = AOS_ERR_NOT_FOUND; break; }
        MockInstance *inst = &g_instances[slot];
        /* Check not already attached */
        for (uint32_t d = 0; d < MOCK_MAX_DEVICES; d++) {
            if (inst->devs[d].attached &&
                inst->devs[d].dev_kind == dev_kind &&
                inst->devs[d].dev_id   == dev_id) {
                _mrs[0] = AOS_ERR_EXISTS; return;
            }
        }
        /* Find a free slot */
        int dslot = -1;
        for (uint32_t d = 0; d < MOCK_MAX_DEVICES; d++) {
            if (!inst->devs[d].attached) { dslot = (int)d; break; }
        }
        if (dslot < 0) { _mrs[0] = AOS_ERR_NOSPC; break; }
        inst->devs[dslot].dev_kind = dev_kind;
        inst->devs[dslot].dev_id   = dev_id;
        inst->devs[dslot].attached = 1u;
        _mrs[0] = AOS_OK;
        break;
    }

    /* ── DETACH ─────────────────────────────────────────────────────────── */
    case OP_VOS_DETACH: {
        uint32_t id       = (uint32_t)_mrs[1];
        uint32_t dev_kind = (uint32_t)_mrs[2];
        uint32_t dev_id   = (uint32_t)_mrs[3];
        int slot = vos_find(id);
        if (slot < 0) { _mrs[0] = AOS_ERR_NOT_FOUND; break; }
        MockInstance *inst = &g_instances[slot];
        bool found = false;
        for (uint32_t d = 0; d < MOCK_MAX_DEVICES; d++) {
            if (inst->devs[d].attached &&
                inst->devs[d].dev_kind == dev_kind &&
                inst->devs[d].dev_id   == dev_id) {
                inst->devs[d].attached = 0u;
                found = true;
                break;
            }
        }
        _mrs[0] = found ? AOS_OK : AOS_ERR_NOT_FOUND;
        break;
    }

    /* ── CONFIGURE ─────────────────────────────────────────────────────── */
    case OP_VOS_CONFIGURE: {
        uint32_t id            = (uint32_t)_mrs[1];
        uint32_t new_mem       = (uint32_t)_mrs[2];
        uint32_t new_budget_us = (uint32_t)_mrs[3];
        uint32_t new_period_us = (uint32_t)_mrs[4];
        int slot = vos_find(id);
        if (slot < 0) { _mrs[0] = AOS_ERR_NOT_FOUND; break; }
        if (new_mem)       g_instances[slot].mem_mb        = new_mem;
        if (new_budget_us) g_instances[slot].cpu_budget_us = new_budget_us;
        if (new_period_us) g_instances[slot].cpu_period_us = new_period_us;
        _mrs[0] = AOS_OK;
        break;
    }

    default:
        _mrs[0] = AOS_ERR_UNIMPL;
        break;
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static uint32_t do_create(const char *name, uint32_t img,
                          uint32_t mb, uint32_t cpus) {
    mock_mr_clear();
    _mrs[0] = OP_VOS_CREATE;
    _mrs[1] = (uint64_t)(uintptr_t)name;
    _mrs[2] = (uint64_t)strlen(name);
    _mrs[3] = img; _mrs[4] = mb; _mrs[5] = cpus;
    vos_dispatch(0, 0);
    return (_mrs[0] == AOS_OK) ? (uint32_t)_mrs[1] : 0u;
}

/* Force state of an existing instance for testing DESTROY/STATUS edge cases */
static void set_state(uint32_t id, uint32_t state) {
    int slot = vos_find(id);
    if (slot >= 0) g_instances[slot].state = state;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Tests
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_create_linux_ok(void) {
    vos_reset();
    uint32_t id = do_create("linux-guest", VOS_IMAGE_LINUX, 512, 2);
    ASSERT_NE(id, 0u, "VOS_CREATE: Linux image returns non-zero id");
    ASSERT_EQ(_mrs[0], AOS_OK, "VOS_CREATE: status AOS_OK");
}

static void test_create_freebsd_ok(void) {
    vos_reset();
    uint32_t id = do_create("bsd-guest", VOS_IMAGE_FREEBSD, 256, 1);
    ASSERT_NE(id, 0u, "VOS_CREATE: FreeBSD image returns non-zero id");
}

static void test_create_ids_unique(void) {
    vos_reset();
    uint32_t id1 = do_create("a", VOS_IMAGE_LINUX,   128, 1);
    uint32_t id2 = do_create("b", VOS_IMAGE_FREEBSD, 128, 1);
    ASSERT_NE(id1, id2, "VOS_CREATE: two instances get distinct ids");
}

static void test_create_null_name(void) {
    vos_reset();
    mock_mr_clear();
    _mrs[0] = OP_VOS_CREATE;
    _mrs[1] = 0;  /* NULL name */
    _mrs[2] = 5;
    _mrs[3] = VOS_IMAGE_LINUX; _mrs[4] = 256; _mrs[5] = 1;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "VOS_CREATE: null name returns AOS_ERR_INVAL");
}

static void test_create_zero_memory(void) {
    vos_reset();
    mock_mr_clear();
    _mrs[0] = OP_VOS_CREATE;
    _mrs[1] = (uint64_t)(uintptr_t)"z";
    _mrs[2] = 1;
    _mrs[3] = VOS_IMAGE_LINUX; _mrs[4] = 0; /* mem_mb = 0 */; _mrs[5] = 1;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "VOS_CREATE: zero mem_mb returns AOS_ERR_INVAL");
}

static void test_create_zero_cpus(void) {
    vos_reset();
    mock_mr_clear();
    _mrs[0] = OP_VOS_CREATE;
    _mrs[1] = (uint64_t)(uintptr_t)"z";
    _mrs[2] = 1;
    _mrs[3] = VOS_IMAGE_LINUX; _mrs[4] = 128; _mrs[5] = 0; /* vcpus = 0 */
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "VOS_CREATE: zero vcpus returns AOS_ERR_INVAL");
}

static void test_create_table_full(void) {
    vos_reset();
    for (uint32_t i = 0; i < MOCK_MAX_INSTANCES; i++) {
        char name[8]; snprintf(name, sizeof(name), "g%u", i);
        do_create(name, VOS_IMAGE_LINUX, 128, 1);
    }
    uint32_t id = do_create("overflow", VOS_IMAGE_LINUX, 128, 1);
    ASSERT_EQ(id, 0u, "VOS_CREATE: full table returns error (id==0)");
    ASSERT_EQ(_mrs[0], AOS_ERR_NOSPC, "VOS_CREATE: full table returns AOS_ERR_NOSPC");
}

static void test_destroy_stopped_ok(void) {
    vos_reset();
    uint32_t id = do_create("tmp", VOS_IMAGE_LINUX, 128, 1);
    /* Default state is STOPPED */
    mock_mr_clear();
    _mrs[0] = OP_VOS_DESTROY; _mrs[1] = id;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "VOS_DESTROY: stopped instance returns AOS_OK");
}

static void test_destroy_running_fails(void) {
    vos_reset();
    uint32_t id = do_create("live", VOS_IMAGE_LINUX, 256, 2);
    set_state(id, VOS_STATE_RUNNING);
    mock_mr_clear();
    _mrs[0] = OP_VOS_DESTROY; _mrs[1] = id;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_BUSY,
              "VOS_DESTROY: running instance returns AOS_ERR_BUSY");
}

static void test_destroy_unknown_id(void) {
    vos_reset();
    mock_mr_clear();
    _mrs[0] = OP_VOS_DESTROY; _mrs[1] = 9999u;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
              "VOS_DESTROY: unknown id returns AOS_ERR_NOT_FOUND");
}

static void test_destroy_removes_from_list(void) {
    vos_reset();
    uint32_t id = do_create("r", VOS_IMAGE_LINUX, 128, 1);
    mock_mr_clear(); _mrs[0] = OP_VOS_DESTROY; _mrs[1] = id;
    vos_dispatch(0, 0);
    mock_mr_clear(); _mrs[0] = OP_VOS_LIST;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[1], 0u, "VOS_DESTROY: list count drops to 0 after destroy");
}

static void test_status_ok(void) {
    vos_reset();
    uint32_t id = do_create("s", VOS_IMAGE_FREEBSD, 512, 4);
    mock_mr_clear();
    _mrs[0] = OP_VOS_STATUS; _mrs[1] = id;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "VOS_STATUS: returns AOS_OK");
    ASSERT_EQ(_mrs[1], VOS_STATE_STOPPED, "VOS_STATUS: new instance is STOPPED");
    ASSERT_EQ(_mrs[2], 512u, "VOS_STATUS: mem_mb preserved");
    ASSERT_EQ(_mrs[3], 4u,   "VOS_STATUS: vcpus preserved");
}

static void test_status_after_state_change(void) {
    vos_reset();
    uint32_t id = do_create("t", VOS_IMAGE_LINUX, 256, 1);
    set_state(id, VOS_STATE_RUNNING);
    mock_mr_clear();
    _mrs[0] = OP_VOS_STATUS; _mrs[1] = id;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[1], VOS_STATE_RUNNING, "VOS_STATUS: reports RUNNING after state change");
}

static void test_status_unknown_id(void) {
    vos_reset();
    mock_mr_clear();
    _mrs[0] = OP_VOS_STATUS; _mrs[1] = 7777u;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
              "VOS_STATUS: unknown id returns AOS_ERR_NOT_FOUND");
}

static void test_list_empty(void) {
    vos_reset();
    mock_mr_clear(); _mrs[0] = OP_VOS_LIST;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "VOS_LIST: empty returns AOS_OK");
    ASSERT_EQ(_mrs[1], 0u, "VOS_LIST: count == 0 on empty roster");
}

static void test_list_counts_instances(void) {
    vos_reset();
    do_create("a", VOS_IMAGE_LINUX,   128, 1);
    do_create("b", VOS_IMAGE_FREEBSD, 256, 2);
    mock_mr_clear(); _mrs[0] = OP_VOS_LIST;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[1], 2u, "VOS_LIST: count == 2 after two creates");
}

static void test_attach_serial_ok(void) {
    vos_reset();
    uint32_t id = do_create("att", VOS_IMAGE_LINUX, 128, 1);
    mock_mr_clear();
    _mrs[0] = OP_VOS_ATTACH; _mrs[1] = id;
    _mrs[2] = VOS_DEV_SERIAL; _mrs[3] = 0;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "VOS_ATTACH: attach serial returns AOS_OK");
}

static void test_attach_block_ok(void) {
    vos_reset();
    uint32_t id = do_create("blk", VOS_IMAGE_LINUX, 128, 1);
    mock_mr_clear();
    _mrs[0] = OP_VOS_ATTACH; _mrs[1] = id;
    _mrs[2] = VOS_DEV_BLOCK; _mrs[3] = 1;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "VOS_ATTACH: attach block device returns AOS_OK");
}

static void test_attach_duplicate(void) {
    vos_reset();
    uint32_t id = do_create("dup", VOS_IMAGE_LINUX, 128, 1);
    mock_mr_clear();
    _mrs[0] = OP_VOS_ATTACH; _mrs[1] = id; _mrs[2] = VOS_DEV_NET; _mrs[3] = 0;
    vos_dispatch(0, 0);
    mock_mr_clear();
    _mrs[0] = OP_VOS_ATTACH; _mrs[1] = id; _mrs[2] = VOS_DEV_NET; _mrs[3] = 0;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_EXISTS,
              "VOS_ATTACH: duplicate attach returns AOS_ERR_EXISTS");
}

static void test_attach_unknown_instance(void) {
    vos_reset();
    mock_mr_clear();
    _mrs[0] = OP_VOS_ATTACH; _mrs[1] = 8888u;
    _mrs[2] = VOS_DEV_SERIAL; _mrs[3] = 0;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
              "VOS_ATTACH: unknown instance returns AOS_ERR_NOT_FOUND");
}

static void test_attach_device_table_full(void) {
    vos_reset();
    uint32_t id = do_create("full", VOS_IMAGE_LINUX, 128, 1);
    for (uint32_t d = 0; d < MOCK_MAX_DEVICES; d++) {
        mock_mr_clear();
        _mrs[0] = OP_VOS_ATTACH; _mrs[1] = id;
        _mrs[2] = VOS_DEV_BLOCK; _mrs[3] = d;
        vos_dispatch(0, 0);
    }
    mock_mr_clear();
    _mrs[0] = OP_VOS_ATTACH; _mrs[1] = id;
    _mrs[2] = VOS_DEV_NET; _mrs[3] = 99;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOSPC,
              "VOS_ATTACH: full device table returns AOS_ERR_NOSPC");
}

static void test_detach_ok(void) {
    vos_reset();
    uint32_t id = do_create("det", VOS_IMAGE_LINUX, 128, 1);
    mock_mr_clear();
    _mrs[0] = OP_VOS_ATTACH; _mrs[1] = id; _mrs[2] = VOS_DEV_SERIAL; _mrs[3] = 0;
    vos_dispatch(0, 0);

    mock_mr_clear();
    _mrs[0] = OP_VOS_DETACH; _mrs[1] = id; _mrs[2] = VOS_DEV_SERIAL; _mrs[3] = 0;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "VOS_DETACH: detach attached device returns AOS_OK");
}

static void test_detach_not_attached(void) {
    vos_reset();
    uint32_t id = do_create("x", VOS_IMAGE_LINUX, 128, 1);
    mock_mr_clear();
    _mrs[0] = OP_VOS_DETACH; _mrs[1] = id; _mrs[2] = VOS_DEV_NET; _mrs[3] = 99;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
              "VOS_DETACH: non-existent device returns AOS_ERR_NOT_FOUND");
}

static void test_detach_unknown_instance(void) {
    vos_reset();
    mock_mr_clear();
    _mrs[0] = OP_VOS_DETACH; _mrs[1] = 7777u; _mrs[2] = VOS_DEV_SERIAL; _mrs[3] = 0;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
              "VOS_DETACH: unknown instance returns AOS_ERR_NOT_FOUND");
}

static void test_configure_mem_ok(void) {
    vos_reset();
    uint32_t id = do_create("cfg", VOS_IMAGE_LINUX, 256, 2);
    mock_mr_clear();
    _mrs[0] = OP_VOS_CONFIGURE; _mrs[1] = id;
    _mrs[2] = 512; _mrs[3] = 0; _mrs[4] = 0;  /* update mem_mb to 512 */
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "VOS_CONFIGURE: returns AOS_OK");
    /* verify via STATUS */
    mock_mr_clear(); _mrs[0] = OP_VOS_STATUS; _mrs[1] = id;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[2], 512u, "VOS_CONFIGURE: mem_mb updated");
}

static void test_configure_bad_id(void) {
    vos_reset();
    mock_mr_clear();
    _mrs[0] = OP_VOS_CONFIGURE; _mrs[1] = 9999u;
    _mrs[2] = 512; _mrs[3] = 0; _mrs[4] = 0;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
              "VOS_CONFIGURE: unknown id returns AOS_ERR_NOT_FOUND");
}

static void test_configure_noop_ok(void) {
    vos_reset();
    uint32_t id = do_create("noop", VOS_IMAGE_FREEBSD, 128, 1);
    mock_mr_clear();
    _mrs[0] = OP_VOS_CONFIGURE; _mrs[1] = id;
    _mrs[2] = 0; _mrs[3] = 0; _mrs[4] = 0;  /* all-zero = no change */
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "VOS_CONFIGURE: all-zero params returns AOS_OK");
    mock_mr_clear(); _mrs[0] = OP_VOS_STATUS; _mrs[1] = id;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[2], 128u, "VOS_CONFIGURE: noop preserves mem_mb");
}

static void test_unknown_opcode(void) {
    vos_reset();
    mock_mr_clear();
    _mrs[0] = 0xDDu;
    vos_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_UNIMPL, "unknown opcode returns AOS_ERR_UNIMPL");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    TAP_PLAN(43);

    test_create_linux_ok();
    test_create_freebsd_ok();
    test_create_ids_unique();
    test_create_null_name();
    test_create_zero_memory();
    test_create_zero_cpus();
    test_create_table_full();

    test_destroy_stopped_ok();
    test_destroy_running_fails();
    test_destroy_unknown_id();
    test_destroy_removes_from_list();

    test_status_ok();
    test_status_after_state_change();
    test_status_unknown_id();

    test_list_empty();
    test_list_counts_instances();

    test_attach_serial_ok();
    test_attach_block_ok();
    test_attach_duplicate();
    test_attach_unknown_instance();
    test_attach_device_table_full();

    test_detach_ok();
    test_detach_not_attached();
    test_detach_unknown_instance();

    test_configure_mem_ok();
    test_configure_bad_id();
    test_configure_noop_ok();

    test_unknown_opcode();

    /* Full lifecycle round-trip: create → attach net → status → detach → destroy */
    {
        vos_reset();
        uint32_t id = do_create("lifecycle", VOS_IMAGE_LINUX, 1024, 4);
        ASSERT_NE(id, 0u, "lifecycle: instance created");

        mock_mr_clear();
        _mrs[0] = OP_VOS_ATTACH; _mrs[1] = id; _mrs[2] = VOS_DEV_NET; _mrs[3] = 0;
        vos_dispatch(0, 0);
        ASSERT_EQ(_mrs[0], AOS_OK, "lifecycle: net attached");

        mock_mr_clear();
        _mrs[0] = OP_VOS_STATUS; _mrs[1] = id;
        vos_dispatch(0, 0);
        ASSERT_EQ(_mrs[1], VOS_STATE_STOPPED, "lifecycle: state is STOPPED");

        mock_mr_clear();
        _mrs[0] = OP_VOS_DETACH; _mrs[1] = id; _mrs[2] = VOS_DEV_NET; _mrs[3] = 0;
        vos_dispatch(0, 0);
        ASSERT_EQ(_mrs[0], AOS_OK, "lifecycle: net detached");

        mock_mr_clear();
        _mrs[0] = OP_VOS_DESTROY; _mrs[1] = id;
        vos_dispatch(0, 0);
        ASSERT_EQ(_mrs[0], AOS_OK, "lifecycle: instance destroyed");
    }

    /* VOS_LIST after destroy returns decremented count */
    {
        vos_reset();
        uint32_t a = do_create("x", VOS_IMAGE_LINUX, 128, 1);
        uint32_t b = do_create("y", VOS_IMAGE_LINUX, 128, 1);
        mock_mr_clear(); _mrs[0] = OP_VOS_DESTROY; _mrs[1] = a;
        vos_dispatch(0, 0);
        mock_mr_clear(); _mrs[0] = OP_VOS_LIST;
        vos_dispatch(0, 0);
        ASSERT_EQ(_mrs[1], 1u, "VOS_LIST: count == 1 after destroying one of two");
        (void)b;
    }

    /* Attach then re-attach after detach succeeds */
    {
        vos_reset();
        uint32_t id = do_create("reattach", VOS_IMAGE_FREEBSD, 256, 2);
        mock_mr_clear(); _mrs[0] = OP_VOS_ATTACH;
        _mrs[1] = id; _mrs[2] = VOS_DEV_BLOCK; _mrs[3] = 0;
        vos_dispatch(0, 0);

        mock_mr_clear(); _mrs[0] = OP_VOS_DETACH;
        _mrs[1] = id; _mrs[2] = VOS_DEV_BLOCK; _mrs[3] = 0;
        vos_dispatch(0, 0);

        mock_mr_clear(); _mrs[0] = OP_VOS_ATTACH;
        _mrs[1] = id; _mrs[2] = VOS_DEV_BLOCK; _mrs[3] = 0;
        vos_dispatch(0, 0);
        ASSERT_EQ(_mrs[0], AOS_OK,
                  "VOS_ATTACH: re-attach after detach succeeds");
    }

    return tap_exit();
}

#else
typedef int _agentos_api_test_vibeos_dummy;
#endif /* AGENTOS_TEST_HOST */
