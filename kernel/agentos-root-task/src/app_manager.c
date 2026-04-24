/*
 * app_manager.c — AppManager Protection Domain
 *
 * Active PD (priority 130, console slot 18).
 *
 * Orchestrates full-stack application deployment:
 *   1. Reads key=value manifests from app_manifest_shmem
 *   2. Creates a vNIC via NetServer (OP_NET_VNIC_CREATE)
 *   3. Writes app name + ELF path to spawn_config_shmem and calls
 *      SpawnServer (OP_SPAWN_LAUNCH)
 *   4. Registers the app's HTTP URL prefix with http_svc (OP_HTTP_REGISTER)
 *
 * On OP_APP_KILL the reverse sequence is performed: route unregistered,
 * spawn slot killed, vNIC destroyed.
 *
 * Outbound endpoints (resolved at startup or left 0 for test builds):
 *   g_spawn_ep   — SpawnServer IPC endpoint
 *   g_net_ep     — NetServer IPC endpoint
 *   g_http_ep    — http_svc IPC endpoint
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "agentos.h"
#include "sel4_server.h"
#include "app_manager.h"
#include "spawn.h"
#include "net_server.h"
#include "http_svc.h"

/* ── Shared memory globals (set via setvar_vaddr) ────────────────────────── */
uintptr_t app_manifest_shmem_vaddr;
uintptr_t spawn_config_shmem_appmgr_vaddr;
uintptr_t vfs_io_shmem_appmgr_vaddr;   /* reserved for future VFS reads */

/* Weak console_rings fallback */
uintptr_t log_drain_rings_vaddr;

/* ── Outbound endpoints (0 = not wired; outbound calls skipped) ──────────── */
static seL4_CPtr g_spawn_ep = 0;
static seL4_CPtr g_net_ep   = 0;
static seL4_CPtr g_http_ep  = 0;

/* ── Simple debug logging ────────────────────────────────────────────────── */
#define LOG(msg)  log_drain_write(APP_MANAGER_CONSOLE_SLOT, APP_MANAGER_PD_ID, \
                              "[app_manager] " msg "\n")

/* ── Application tracking table ──────────────────────────────────────────── */
typedef struct {
    bool     active;
    uint32_t app_id;
    uint32_t vnic_id;
    uint32_t spawn_slot;
    uint32_t cap_classes;
    uint32_t state;
    char     name[APP_MANIFEST_NAME_MAX];
    char     http_prefix[APP_MANIFEST_PREFIX_MAX];
} app_entry_t;

static app_entry_t apps[APP_MAX_APPS];
static uint32_t    next_app_id = 1;

/* ── msg helpers ──────────────────────────────────────────────────────────── */
static inline uint32_t msg_u32(const sel4_msg_t *m, uint32_t off) {
    uint32_t v = 0;
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        v  = (uint32_t)m->data[off]; v |= (uint32_t)m->data[off+1]<<8;
        v |= (uint32_t)m->data[off+2]<<16; v |= (uint32_t)m->data[off+3]<<24;
    }
    return v;
}
static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        m->data[off]=(uint8_t)v; m->data[off+1]=(uint8_t)(v>>8);
        m->data[off+2]=(uint8_t)(v>>16); m->data[off+3]=(uint8_t)(v>>24);
    }
}

/* ── String helpers (no libc) ────────────────────────────────────────────── */
static void s_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i + 1 < max && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static uint32_t s_len(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static bool s_eq(const char *a, const char *b, uint32_t blen)
{
    for (uint32_t i = 0; i < blen; i++) {
        if (a[i] != b[i]) return false;
    }
    return a[blen] == '\0';
}

/* ── Manifest parser ─────────────────────────────────────────────────────── */
static int parse_manifest(const char *text, uint32_t len, app_manifest_t *out)
{
    out->name[0] = '\0';
    out->elf_path[0] = '\0';
    out->http_prefix[0] = '\0';
    out->cap_classes = APP_DEFAULT_CAPS;

    const char *p   = text;
    const char *end = text + len;

    while (p < end) {
        const char *ks = p;
        while (p < end && *p != '=' && *p != '\n' && *p != '\r') p++;
        if (p >= end || *p != '=') {
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
            continue;
        }
        uint32_t klen = (uint32_t)(p - ks);
        p++;

        const char *vs = p;
        while (p < end && *p != '\n' && *p != '\r') p++;
        uint32_t vlen = (uint32_t)(p - vs);
        while (p < end && (*p == '\n' || *p == '\r')) p++;

        if (klen == 0 || vlen == 0) continue;

        char val[256];
        uint32_t vl = vlen < 255 ? vlen : 255;
        for (uint32_t i = 0; i < vl; i++) val[i] = vs[i];
        val[vl] = '\0';

        if (s_eq("name", ks, klen))
            s_copy(out->name, val, APP_MANIFEST_NAME_MAX);
        else if (s_eq("elf", ks, klen))
            s_copy(out->elf_path, val, APP_MANIFEST_ELF_MAX);
        else if (s_eq("http_prefix", ks, klen))
            s_copy(out->http_prefix, val, APP_MANIFEST_PREFIX_MAX);
        else if (s_eq("caps", ks, klen)) {
            uint32_t caps = 0;
            for (uint32_t i = 0; val[i] >= '0' && val[i] <= '9'; i++)
                caps = caps * 10u + (uint32_t)(val[i] - '0');
            if (caps) out->cap_classes = caps;
        }
    }

    return (out->name[0] != '\0' && out->elf_path[0] != '\0') ? 0 : -1;
}

/* ── App table helpers ───────────────────────────────────────────────────── */
static app_entry_t *alloc_slot(void)
{
    for (uint32_t i = 0; i < APP_MAX_APPS; i++)
        if (!apps[i].active) return &apps[i];
    return (void *)0;
}

static app_entry_t *find_by_id(uint32_t id)
{
    for (uint32_t i = 0; i < APP_MAX_APPS; i++)
        if (apps[i].active && apps[i].app_id == id) return &apps[i];
    return (void *)0;
}

static uint32_t count_running(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < APP_MAX_APPS; i++)
        n += (apps[i].active && apps[i].state == APP_STATE_RUNNING) ? 1u : 0u;
    return n;
}

/* ── HTTP prefix packing into sel4_msg_t ─────────────────────────────────── */
/*
 * HTTP_PREFIX_MR_BASE=4, HTTP_PREFIX_MR_COUNT=8.
 * Each "word" is a uint32 at byte offset (base+w)*4 in data[].
 * Prefix bytes are packed little-endian within each word.
 */
static void pack_prefix_into_msg(sel4_msg_t *m, const char *prefix, uint32_t len)
{
    for (uint32_t w = 0; w < HTTP_PREFIX_MR_COUNT; w++) {
        uint32_t word = 0;
        uint32_t byte_off = (HTTP_PREFIX_MR_BASE + w) * 4u;
        for (uint32_t b = 0; b < 4; b++) {
            uint32_t idx = w * 4u + b;
            if (idx < len)
                word |= (uint32_t)(uint8_t)prefix[idx] << (b * 8u);
        }
        if (byte_off + 4u <= SEL4_MSG_DATA_BYTES) {
            m->data[byte_off]   = (uint8_t)word;
            m->data[byte_off+1] = (uint8_t)(word >> 8);
            m->data[byte_off+2] = (uint8_t)(word >> 16);
            m->data[byte_off+3] = (uint8_t)(word >> 24);
        }
    }
}

/* ── Spawn config staging ────────────────────────────────────────────────── */
static void stage_spawn_config(const char *name, const char *elf_path)
{
    if (!spawn_config_shmem_appmgr_vaddr) return;
    volatile uint8_t *cfg = (volatile uint8_t *)spawn_config_shmem_appmgr_vaddr;

    for (uint32_t i = 0; i < SPAWN_CONFIG_NAME_MAX; i++)
        cfg[SPAWN_CONFIG_NAME_OFF + i] = (i < s_len(name)) ? (uint8_t)name[i] : 0u;

    for (uint32_t i = 0; i < SPAWN_CONFIG_PATH_MAX; i++)
        cfg[SPAWN_CONFIG_PATH_OFF + i] = (i < s_len(elf_path)) ? (uint8_t)elf_path[i] : 0u;
}

/* ── Outbound IPC helper ─────────────────────────────────────────────────── */
/*
 * outbound_call — make a seL4_Call to an outbound endpoint.
 * Returns the result code from rep->data[0..3], or 0xFFFFFFFF if ep==0.
 * Guarded with AGENTOS_TEST_HOST so test builds never block.
 */
static uint32_t outbound_call(seL4_CPtr ep, sel4_msg_t *req, sel4_msg_t *rep)
{
#ifndef AGENTOS_TEST_HOST
    if (!ep) return 0xFFFFFFFFu;
    seL4_Call(ep, sel4_msg_to_info(req), rep);
    return msg_u32(rep, 0);
#else
    (void)ep; (void)req;
    /* In test host mode, simulate success */
    rep_u32(rep, 0, 0);
    rep_u32(rep, 4, 0);
    return 0;
#endif
}

/* ── OP_APP_LAUNCH ───────────────────────────────────────────────────────── */
static uint32_t h_launch(sel4_badge_t b, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t manifest_len = msg_u32(req, 4);

    if (!app_manifest_shmem_vaddr ||
        manifest_len == 0 || manifest_len > APP_MANIFEST_TEXT_MAX) {
        rep_u32(rep, 0, APP_ERR_INVAL); rep->length = 4;
        return SEL4_ERR_INVALID_ARG;
    }

    app_entry_t *slot = alloc_slot();
    if (!slot) {
        rep_u32(rep, 0, APP_ERR_NO_SLOTS); rep->length = 4;
        return SEL4_ERR_NO_MEM;
    }

    app_manifest_t manifest;
    if (parse_manifest((const char *)app_manifest_shmem_vaddr,
                       manifest_len, &manifest) < 0) {
        rep_u32(rep, 0, APP_ERR_INVAL); rep->length = 4;
        return SEL4_ERR_INVALID_ARG;
    }

    slot->app_id      = next_app_id++;
    slot->state       = APP_STATE_LOADING;
    slot->cap_classes = manifest.cap_classes;
    slot->vnic_id     = 0xFFFFFFFFu;
    slot->spawn_slot  = 0xFFFFFFFFu;
    s_copy(slot->name,        manifest.name,        APP_MANIFEST_NAME_MAX);
    s_copy(slot->http_prefix, manifest.http_prefix, APP_MANIFEST_PREFIX_MAX);

    /* Step 1: Create vNIC via NetServer */
    {
        sel4_msg_t out = {0};
        sel4_msg_t in  = {0};
        out.opcode = OP_NET_VNIC_CREATE;
        rep_u32(&out, 0, OP_NET_VNIC_CREATE);
        rep_u32(&out, 4, 0xFFu);
        rep_u32(&out, 8, manifest.cap_classes);
        rep_u32(&out, 12, slot->app_id);
        out.length = 16;
        uint32_t rc = outbound_call(g_net_ep, &out, &in);
        if (rc == NET_OK)
            slot->vnic_id = msg_u32(&in, 4);
    }

    /* Step 2: Stage config and call SpawnServer */
    stage_spawn_config(manifest.name, manifest.elf_path);

    uint32_t spawn_rc;
    uint32_t spawn_assigned_id = 0;
    {
        sel4_msg_t out = {0};
        sel4_msg_t in  = {0};
        out.opcode = OP_SPAWN_LAUNCH;
        rep_u32(&out, 0, OP_SPAWN_LAUNCH);
        rep_u32(&out, 4, manifest.cap_classes);
        rep_u32(&out, 8, slot->vnic_id);
        out.length = 12;
        spawn_rc = outbound_call(g_spawn_ep, &out, &in);
        spawn_assigned_id = msg_u32(&in, 4);
    }

    if (spawn_rc != SPAWN_OK) {
        /* Tear down vNIC on spawn failure */
        if (slot->vnic_id != 0xFFFFFFFFu) {
            sel4_msg_t out = {0};
            sel4_msg_t in  = {0};
            out.opcode = OP_NET_VNIC_DESTROY;
            rep_u32(&out, 0, OP_NET_VNIC_DESTROY);
            rep_u32(&out, 4, slot->vnic_id);
            out.length = 8;
            outbound_call(g_net_ep, &out, &in);
        }
        slot->active = false;
        rep_u32(rep, 0, APP_ERR_SPAWN); rep->length = 4;
        return SEL4_ERR_INVALID_ARG;
    }

    slot->spawn_slot = spawn_assigned_id;

    /* Step 3: Register HTTP route (non-fatal) */
    if (manifest.http_prefix[0] != '\0') {
        uint32_t plen = s_len(manifest.http_prefix);
        sel4_msg_t out = {0};
        sel4_msg_t in  = {0};
        out.opcode = OP_HTTP_REGISTER;
        rep_u32(&out, 0, OP_HTTP_REGISTER);
        rep_u32(&out, 4, slot->app_id);
        rep_u32(&out, 8, slot->vnic_id);
        rep_u32(&out, 12, plen);
        pack_prefix_into_msg(&out, manifest.http_prefix, plen);
        out.length = (HTTP_PREFIX_MR_BASE + HTTP_PREFIX_MR_COUNT) * 4u;
        outbound_call(g_http_ep, &out, &in);
    }

    slot->active = true;
    slot->state  = APP_STATE_RUNNING;
    LOG("app launched");

    rep_u32(rep, 0, APP_OK);
    rep_u32(rep, 4, slot->app_id);
    rep_u32(rep, 8, slot->vnic_id);
    rep->length = 12;
    return SEL4_ERR_OK;
}

/* ── OP_APP_KILL ─────────────────────────────────────────────────────────── */
static uint32_t h_kill(sel4_badge_t b, const sel4_msg_t *req,
                         sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t app_id = msg_u32(req, 4);
    app_entry_t *entry = find_by_id(app_id);
    if (!entry) {
        rep_u32(rep, 0, APP_ERR_NOT_FOUND); rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }

    entry->state = APP_STATE_STOPPING;

    /* Kill spawn slot */
    {
        sel4_msg_t out = {0};
        sel4_msg_t in  = {0};
        out.opcode = OP_SPAWN_KILL;
        rep_u32(&out, 0, OP_SPAWN_KILL);
        rep_u32(&out, 4, app_id);
        out.length = 8;
        outbound_call(g_spawn_ep, &out, &in);
    }

    /* Destroy vNIC */
    if (entry->vnic_id != 0xFFFFFFFFu) {
        sel4_msg_t out = {0};
        sel4_msg_t in  = {0};
        out.opcode = OP_NET_VNIC_DESTROY;
        rep_u32(&out, 0, OP_NET_VNIC_DESTROY);
        rep_u32(&out, 4, entry->vnic_id);
        out.length = 8;
        outbound_call(g_net_ep, &out, &in);
    }

    /* Unregister HTTP route */
    if (entry->http_prefix[0] != '\0') {
        sel4_msg_t out = {0};
        sel4_msg_t in  = {0};
        out.opcode = OP_HTTP_UNREGISTER;
        rep_u32(&out, 0, OP_HTTP_UNREGISTER);
        rep_u32(&out, 4, app_id);
        out.length = 8;
        outbound_call(g_http_ep, &out, &in);
    }

    entry->active = false;
    rep_u32(rep, 0, APP_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── OP_APP_STATUS ───────────────────────────────────────────────────────── */
static uint32_t h_status(sel4_badge_t b, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t app_id = msg_u32(req, 4);
    app_entry_t *entry = find_by_id(app_id);
    if (!entry) {
        rep_u32(rep, 0, APP_ERR_NOT_FOUND); rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }

    rep_u32(rep, 0,  APP_OK);
    rep_u32(rep, 4,  entry->state);
    rep_u32(rep, 8,  entry->vnic_id);
    rep_u32(rep, 12, entry->spawn_slot);
    rep->length = 16;
    return SEL4_ERR_OK;
}

/* ── OP_APP_LIST ─────────────────────────────────────────────────────────── */
static uint32_t h_list(sel4_badge_t b, const sel4_msg_t *req,
                         sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    if (!app_manifest_shmem_vaddr) {
        rep_u32(rep, 0, APP_ERR_INVAL); rep->length = 4;
        return SEL4_ERR_INVALID_ARG;
    }

    volatile app_list_entry_t *list =
        (volatile app_list_entry_t *)
        (app_manifest_shmem_vaddr + APP_MANIFEST_LIST_OFF);

    uint32_t count = 0;
    for (uint32_t i = 0; i < APP_MAX_APPS && count < APP_LIST_MAX_ENTRIES; i++) {
        if (!apps[i].active) continue;
        volatile app_list_entry_t *e = &list[count++];
        e->app_id      = apps[i].app_id;
        e->state       = apps[i].state;
        e->vnic_id     = apps[i].vnic_id;
        e->spawn_slot  = apps[i].spawn_slot;
        e->cap_classes = apps[i].cap_classes;
        e->flags       = 0;
        e->launch_tick = 0;
        for (uint32_t j = 0; j < APP_MANIFEST_NAME_MAX; j++)
            e->name[j] = apps[i].name[j];
        for (uint32_t j = 0; j < APP_MANIFEST_PREFIX_MAX; j++)
            e->http_prefix[j] = apps[i].http_prefix[j];
    }

    rep_u32(rep, 0, APP_OK);
    rep_u32(rep, 4, count);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ── OP_APP_HEALTH ───────────────────────────────────────────────────────── */
static uint32_t h_health(sel4_badge_t b, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    rep_u32(rep, 0, APP_OK);
    rep_u32(rep, 4, count_running());
    rep_u32(rep, 8, APP_MANAGER_VERSION);
    rep->length = 12;
    return SEL4_ERR_OK;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void app_manager_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;

    for (uint32_t i = 0; i < APP_MAX_APPS; i++) apps[i].active = false;
    LOG("init complete");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_APP_LAUNCH,  h_launch, (void *)0);
    sel4_server_register(&srv, OP_APP_KILL,    h_kill,   (void *)0);
    sel4_server_register(&srv, OP_APP_STATUS,  h_status, (void *)0);
    sel4_server_register(&srv, OP_APP_LIST,    h_list,   (void *)0);
    sel4_server_register(&srv, OP_APP_HEALTH,  h_health, (void *)0);
    sel4_server_run(&srv);
}
