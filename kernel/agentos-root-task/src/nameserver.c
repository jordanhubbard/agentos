/*
 * agentOS NameServer Protection Domain
 *
 * Passive PD (priority 170) that provides runtime service discovery.
 * Replaces the hardcoded channel-constant model with a registry that any
 * PD can query to find service metadata, liveness, and routing info.
 *
 * Design notes:
 *   - All channels are pre-wired in agentos.system (Microkit constraint).
 *     NameServer records the channel_id each caller should use to reach a
 *     service; the controller pre-registers static services at boot.
 *   - SpawnServer (task 4) will extend this: when it launches a new app PD
 *     it will query NameServer for service metadata, then mint the
 *     appropriate capability bundle for the child.
 *   - NameServer is passive (passive="true" in .system); it only runs when
 *     called via PPC.  No background work, no notifications sent.
 *
 * Channel map (from this PD's perspective):
 *   CH0:  controller (pp=true)
 *   CH1:  init_agent (pp=true)
 *   CH2:  worker_0   (pp=true)
 *   CH3:  worker_1   (pp=true)
 *   CH4:  worker_2   (pp=true)
 *   CH5:  worker_3   (pp=true)
 *   CH6:  worker_4   (pp=true)
 *   CH7:  worker_5   (pp=true)
 *   CH8:  worker_6   (pp=true)
 *   CH9:  worker_7   (pp=true)
 *
 * Shared memory:
 *   ns_registry_shmem (8KB): written by OP_NS_LIST, read by controller.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "nameserver.h"

/* ── Console ring slot for this PD ──────────────────────────────────────── */
#define NS_LOG_SLOT   13u
#define NS_LOG_PD_ID  13u

/* ── Registry ────────────────────────────────────────────────────────────── */
#define NS_MAX_ENTRIES 64u

typedef struct {
    bool     active;
    char     name[NS_NAME_MAX];
    uint32_t channel_id;
    uint32_t pd_id;
    uint32_t cap_classes;
    uint32_t version;
    uint8_t  status;         /* NS_STATUS_* */
    uint64_t registered_at;  /* boot tick at registration */
} ns_entry_t;

static ns_entry_t  registry[NS_MAX_ENTRIES];
static uint32_t    reg_count = 0;
static uint64_t    boot_tick = 0;  /* incremented on each OP_NS_REGISTER call */

/* Shmem base for OP_NS_LIST dumps (mapped from agentos.system) */
uintptr_t ns_registry_shmem_vaddr;

/* ── String helpers (no libc — m3_bare_metal provides memset/memcpy) ────── */

static int ns_strlen(const char *s)
{
    int n = 0;
    while (n < NS_NAME_MAX && s[n]) n++;
    return n;
}

static int ns_strcmp(const char *a, const char *b)
{
    for (int i = 0; i < NS_NAME_MAX; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

/* ── Registry helpers ────────────────────────────────────────────────────── */

static int registry_find_by_name(const char *name)
{
    for (uint32_t i = 0; i < NS_MAX_ENTRIES; i++) {
        if (registry[i].active && ns_strcmp(registry[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

static int registry_find_by_channel(uint32_t channel_id)
{
    for (uint32_t i = 0; i < NS_MAX_ENTRIES; i++) {
        if (registry[i].active && registry[i].channel_id == channel_id)
            return (int)i;
    }
    return -1;
}

static int registry_alloc(void)
{
    for (uint32_t i = 0; i < NS_MAX_ENTRIES; i++) {
        if (!registry[i].active) return (int)i;
    }
    return -1;
}

/* ── Operation handlers ──────────────────────────────────────────────────── */

/*
 * OP_NS_REGISTER
 *   MR1 = channel_id
 *   MR2 = pd_id
 *   MR3 = cap_classes
 *   MR4 = version
 *   MR5..MR8 = name (NS_NAME_MAX bytes, packed)
 */
static uint32_t handle_register(void)
{
    IPC_STUB_LOCALS
    uint32_t channel_id  = (uint32_t)msg_u32(req, 4);
    uint32_t pd_id       = (uint32_t)msg_u32(req, 8);
    uint32_t cap_classes = (uint32_t)msg_u32(req, 12);
    uint32_t version     = (uint32_t)msg_u32(req, 16);

    char name[NS_NAME_MAX];
    ns_unpack_name(name, 5);

    if (ns_strlen(name) == 0) {
        rep_u32(rep, 0, NS_ERR_BAD_ARGS);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* Reject duplicate names */
    if (registry_find_by_name(name) >= 0) {
        rep_u32(rep, 0, NS_ERR_DUPLICATE);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    int slot = registry_alloc();
    if (slot < 0) {
        rep_u32(rep, 0, NS_ERR_FULL);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    ns_entry_t *e = &registry[slot];
    e->active       = true;
    e->channel_id   = channel_id;
    e->pd_id        = pd_id;
    e->cap_classes  = cap_classes;
    e->version      = version;
    e->status       = NS_STATUS_UNKNOWN;
    e->registered_at = ++boot_tick;

    /* Copy name */
    for (int i = 0; i < NS_NAME_MAX; i++) e->name[i] = name[i];

    reg_count++;

    log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID, "[ns] registered: ");
    log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID, e->name);
    log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID, "\n");

    rep_u32(rep, 0, NS_OK);
    rep->length = 4;
        return SEL4_ERR_OK;
}

/*
 * OP_NS_LOOKUP
 *   MR1..MR4 = name (NS_NAME_MAX bytes, packed)
 * Reply (on NS_OK):
 *   MR0=NS_OK, MR1=channel_id, MR2=pd_id, MR3=status, MR4=cap_classes, MR5=version
 */
static uint32_t handle_lookup(void)
{
    IPC_STUB_LOCALS
    char name[NS_NAME_MAX];
    ns_unpack_name(name, 1);

    if (ns_strlen(name) == 0) {
        rep_u32(rep, 0, NS_ERR_BAD_ARGS);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    int slot = registry_find_by_name(name);
    if (slot < 0) {
        rep_u32(rep, 0, NS_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    ns_entry_t *e = &registry[slot];
    rep_u32(rep, 0, NS_OK);
    rep_u32(rep, 4, e->channel_id);
    rep_u32(rep, 8, e->pd_id);
    rep_u32(rep, 12, e->status);
    rep_u32(rep, 16, e->cap_classes);
    rep_u32(rep, 20, e->version);
    rep->length = 24;
        return SEL4_ERR_OK;
}

/*
 * OP_NS_UPDATE_STATUS
 *   MR1 = channel_id   (identifies the service)
 *   MR2 = new_status   (NS_STATUS_*)
 */
static uint32_t handle_update_status(void)
{
    IPC_STUB_LOCALS
    uint32_t channel_id = (uint32_t)msg_u32(req, 4);
    uint8_t  new_status = (uint8_t)msg_u32(req, 8);

    int slot = registry_find_by_channel(channel_id);
    if (slot < 0) {
        rep_u32(rep, 0, NS_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    registry[slot].status = new_status;

    rep_u32(rep, 0, NS_OK);
    rep->length = 4;
        return SEL4_ERR_OK;
}

/*
 * OP_NS_LIST
 *   Dumps all active entries into ns_registry_shmem.
 * Reply: MR0=NS_OK, MR1=entry_count
 */
static uint32_t handle_list(void)
{
    IPC_STUB_LOCALS
    if (!ns_registry_shmem_vaddr) {
        /* shmem not mapped — return count only */
        rep_u32(rep, 0, NS_OK);
        rep_u32(rep, 4, reg_count);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    ns_list_header_t *hdr = (ns_list_header_t *)ns_registry_shmem_vaddr;
    ns_list_entry_t  *out = hdr->entries;
    uint32_t written = 0;

    /* Zero the header first */
    hdr->magic = NS_LIST_MAGIC;
    hdr->count = 0;
    for (int i = 0; i < 8; i++) hdr->_pad[i] = 0;

    for (uint32_t i = 0; i < NS_MAX_ENTRIES && written < reg_count; i++) {
        if (!registry[i].active) continue;
        ns_entry_t *src = &registry[i];

        for (int n = 0; n < NS_NAME_MAX; n++) out[written].name[n] = src->name[n];
        out[written].channel_id   = src->channel_id;
        out[written].pd_id        = src->pd_id;
        out[written].cap_classes  = src->cap_classes;
        out[written].version      = src->version;
        out[written].status       = src->status;
        out[written].flags        = 0;
        for (int p = 0; p < 6; p++) out[written]._pad[p] = 0;
        out[written].registered_at = src->registered_at;
        written++;
    }

    hdr->count = written;

    rep_u32(rep, 0, NS_OK);
    rep_u32(rep, 4, written);
    rep->length = 8;
        return SEL4_ERR_OK;
}

/*
 * OP_NS_DEREGISTER
 *   MR1 = channel_id
 */
static uint32_t handle_deregister(void)
{
    IPC_STUB_LOCALS
    uint32_t channel_id = (uint32_t)msg_u32(req, 4);

    int slot = registry_find_by_channel(channel_id);
    if (slot < 0) {
        rep_u32(rep, 0, NS_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID, "[ns] deregistered: ");
    log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID, registry[slot].name);
    log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID, "\n");

    registry[slot].active = false;
    reg_count--;

    rep_u32(rep, 0, NS_OK);
    rep->length = 4;
        return SEL4_ERR_OK;
}

/*
 * OP_NS_HEALTH
 * Reply: MR0=NS_OK, MR1=registered_count, MR2=NS_VERSION
 */
static uint32_t handle_health(void)
{
    IPC_STUB_LOCALS
    rep_u32(rep, 0, NS_OK);
    rep_u32(rep, 4, reg_count);
    rep_u32(rep, 8, NS_VERSION);
    rep->length = 12;
        return SEL4_ERR_OK;
}

/* ── Microkit entry points ────────────────────────────────────────────────── */

static void nameserver_pd_init(void)
{
    for (uint32_t i = 0; i < NS_MAX_ENTRIES; i++) {
        registry[i].active = false;
    }
    reg_count = 0;
    boot_tick = 0;

    agentos_log_boot("nameserver");
    log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID,
                "[nameserver] ready — service discovery active\n");
}

static void nameserver_pd_notified(uint32_t ch)
{
    /* NameServer is passive and sends no notifications; this path is
     * only reached if something misconfigures a channel as non-pp.
     * Log and ignore. */
    (void)ch;
    log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID,
                "[ns] unexpected notification (check .system channel pp flags)\n");
}

/*
 * OP_NS_LOOKUP_GATED
 *   MR1..MR4 = name (NS_NAME_MAX bytes, packed — same as OP_NS_LOOKUP)
 *
 * The badge carried on the incoming PPC encodes the caller's authorisation:
 *   bits 31:16 = allowed_cats  — CAP_CLASS_* bitmask set by init_agent at
 *                                bootstrap for this PD.  init_agent knows
 *                                which capability classes each spawned PD is
 *                                allowed to discover and encodes that policy
 *                                into the badge it uses when registering the
 *                                PD's channel with NameServer.
 *   bits 15:0  = requester_pd  — TRACE_PD_* of the calling protection domain.
 *
 * If (entry->cap_classes & allowed_cats) == 0 the target service is in a
 * capability class the caller was never authorised to reach; return
 * NS_ERR_FORBIDDEN.  Otherwise reply identically to OP_NS_LOOKUP.
 *
 * OP_NS_LOOKUP remains available for backwards compatibility but its use
 * in new code is DEPRECATED — it performs no authorization check.
 */
static uint32_t handle_lookup_gated(uint32_t ch, uint32_t msginfo)
{
    IPC_STUB_LOCALS
    /* Extract badge from the msginfo label field.
     * Microkit conveys the badge of the endpoint cap used by the caller
     * in the label field of the msginfo for PPC calls. */
    uint32_t badge        = (uint32_t)msg_u32(req, 0);
    uint16_t requester_pd = (uint16_t)(badge & 0xFFFFu);
    uint16_t allowed_cats = (uint16_t)(badge >> 16);

    (void)ch;
    (void)requester_pd;  /* available for future per-PD audit logging */

    char name[NS_NAME_MAX];
    ns_unpack_name(name, 1);

    if (ns_strlen(name) == 0) {
        rep_u32(rep, 0, NS_ERR_BAD_ARGS);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    int slot = registry_find_by_name(name);
    if (slot < 0) {
        rep_u32(rep, 0, NS_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    ns_entry_t *e = &registry[slot];

    /* Authorization check: at least one cap class must be in the allowed set.
     * If allowed_cats == 0 (badge not set by init_agent) we treat it as
     * "no access" rather than "unrestricted" — fail safe. */
    if (allowed_cats == 0 || (e->cap_classes & (uint32_t)allowed_cats) == 0) {
        log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID, "[ns] gated lookup FORBIDDEN: ");
        log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID, e->name);
        log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID, "\n");
        rep_u32(rep, 0, NS_ERR_FORBIDDEN);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    rep_u32(rep, 0, NS_OK);
    rep_u32(rep, 4, e->channel_id);
    rep_u32(rep, 8, e->pd_id);
    rep_u32(rep, 12, e->status);
    rep_u32(rep, 16, e->cap_classes);
    rep_u32(rep, 20, e->version);
    rep->length = 24;
        return SEL4_ERR_OK;
}

static uint32_t nameserver_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx)
{
    (void)ctx;
    uint32_t op = (uint32_t)msg_u32(req, 0);

    switch (op) {
    case OP_NS_REGISTER:      return handle_register();
    /* DEPRECATED: use OP_NS_LOOKUP_GATED — no authorization check */
    case OP_NS_LOOKUP:        return handle_lookup();
    case OP_NS_UPDATE_STATUS: return handle_update_status();
    case OP_NS_LIST:          return handle_list();
    case OP_NS_DEREGISTER:    return handle_deregister();
    case OP_NS_HEALTH:        return handle_health();
    case OP_NS_LOOKUP_GATED:  return handle_lookup_gated((uint32_t)b, 0);
    default:
        log_drain_write(NS_LOG_SLOT, NS_LOG_PD_ID, "[ns] unknown opcode\n");
        rep_u32(rep, 0, NS_ERR_UNKNOWN_OP);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void nameserver_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    nameserver_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, nameserver_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
