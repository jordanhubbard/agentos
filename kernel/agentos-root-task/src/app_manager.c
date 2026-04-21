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
 * Channel layout (from this PD's perspective):
 *   APP_CH_CONTROLLER (0): controller PPCs in (pp=true)
 *   APP_CH_SPAWN      (1): outgoing PPCs → SpawnServer
 *   APP_CH_VFS        (2): outgoing PPCs → VFS Server (reserved, future use)
 *   APP_CH_NET        (3): outgoing PPCs → NetServer
 *   APP_CH_HTTP       (4): outgoing PPCs → http_svc
 */

#include <microkit.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "agentos.h"
#include "contracts/app_manager_contract.h"
#include "app_manager.h"
#include "spawn.h"
#include "net_server.h"
#include "http_svc.h"

/* ── Shared memory globals (set by Microkit via setvar_vaddr) ────────────── */
uintptr_t app_manifest_shmem_vaddr;
uintptr_t spawn_config_shmem_appmgr_vaddr;
uintptr_t vfs_io_shmem_appmgr_vaddr;   /* reserved for future VFS reads */

/* Weak console_rings fallback — log.c provides __attribute__((weak)) */
uintptr_t log_drain_rings_vaddr;

/* ── Simple debug logging ────────────────────────────────────────────────── */
#define LOG(msg)  log_drain_write(APP_MANAGER_CONSOLE_SLOT, APP_MANAGER_PD_ID, \
                              "[app_manager] " msg "\n")

/* ── Application tracking table ──────────────────────────────────────────── */
typedef struct {
    bool     active;
    uint32_t app_id;
    uint32_t vnic_id;
    uint32_t spawn_slot;     /* slot index from SpawnServer */
    uint32_t cap_classes;
    uint32_t state;          /* APP_STATE_* */
    char     name[APP_MANIFEST_NAME_MAX];
    char     http_prefix[APP_MANIFEST_PREFIX_MAX];
} app_entry_t;

static app_entry_t apps[APP_MAX_APPS];
static uint32_t    next_app_id = 1;

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
/*
 * parse_manifest — extract fields from a key=value text block.
 * Returns 0 on success (both name and elf are present), -1 otherwise.
 */
static int parse_manifest(const char *text, uint32_t len, app_manifest_t *out)
{
    out->name[0] = '\0';
    out->elf_path[0] = '\0';
    out->http_prefix[0] = '\0';
    out->cap_classes = APP_DEFAULT_CAPS;

    const char *p   = text;
    const char *end = text + len;

    while (p < end) {
        /* Locate '=' separating key from value */
        const char *ks = p;
        while (p < end && *p != '=' && *p != '\n' && *p != '\r') p++;
        if (p >= end || *p != '=') {
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
            continue;
        }
        uint32_t klen = (uint32_t)(p - ks);
        p++; /* skip '=' */

        /* Locate end of value */
        const char *vs = p;
        while (p < end && *p != '\n' && *p != '\r') p++;
        uint32_t vlen = (uint32_t)(p - vs);
        while (p < end && (*p == '\n' || *p == '\r')) p++;

        if (klen == 0 || vlen == 0) continue;

        /* Copy value (null-terminated, up to 255 chars) */
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
    return NULL;
}

static app_entry_t *find_by_id(uint32_t id)
{
    for (uint32_t i = 0; i < APP_MAX_APPS; i++)
        if (apps[i].active && apps[i].app_id == id) return &apps[i];
    return NULL;
}

static uint32_t count_running(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < APP_MAX_APPS; i++)
        n += (apps[i].active && apps[i].state == APP_STATE_RUNNING) ? 1u : 0u;
    return n;
}

/* ── HTTP prefix MR packing ──────────────────────────────────────────────── */
/* See HTTP_PREFIX_MR_BASE / HTTP_PREFIX_MR_COUNT in http_svc.h */
static void pack_prefix(const char *prefix, uint32_t len)
{
    for (uint32_t w = 0; w < HTTP_PREFIX_MR_COUNT; w++) {
        uint64_t word = 0;
        for (uint32_t b = 0; b < 8; b++) {
            uint32_t idx = w * 8u + b;
            if (idx < len)
                word |= (uint64_t)(uint8_t)prefix[idx] << (b * 8u);
        }
        microkit_mr_set(HTTP_PREFIX_MR_BASE + w, (seL4_Word)word);
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

/* ── OP_APP_LAUNCH ───────────────────────────────────────────────────────── */
static microkit_msginfo handle_launch(void)
{
    uint32_t manifest_len = (uint32_t)microkit_mr_get(1);

    if (!app_manifest_shmem_vaddr ||
        manifest_len == 0 || manifest_len > APP_MANIFEST_TEXT_MAX)
        return microkit_msginfo_new(APP_ERR_INVAL, 0);

    app_entry_t *slot = alloc_slot();
    if (!slot) return microkit_msginfo_new(APP_ERR_NO_SLOTS, 0);

    app_manifest_t manifest;
    if (parse_manifest((const char *)app_manifest_shmem_vaddr,
                       manifest_len, &manifest) < 0)
        return microkit_msginfo_new(APP_ERR_INVAL, 0);

    /* Initialise slot */
    slot->app_id      = next_app_id++;
    slot->state       = APP_STATE_LOADING;
    slot->cap_classes = manifest.cap_classes;
    slot->vnic_id     = 0xFFFFFFFFu;
    slot->spawn_slot  = 0xFFFFFFFFu;
    s_copy(slot->name,        manifest.name,        APP_MANIFEST_NAME_MAX);
    s_copy(slot->http_prefix, manifest.http_prefix, APP_MANIFEST_PREFIX_MAX);

    /* Step 1: Create vNIC via NetServer (non-fatal on failure) */
    microkit_mr_set(0, OP_NET_VNIC_CREATE);
    microkit_mr_set(1, 0xFFu);               /* auto-assign */
    microkit_mr_set(2, manifest.cap_classes);
    microkit_mr_set(3, slot->app_id);
    microkit_ppcall(APP_CH_NET, microkit_msginfo_new(OP_NET_VNIC_CREATE, 4));
    if ((uint32_t)microkit_mr_get(0) == NET_OK)
        slot->vnic_id = (uint32_t)microkit_mr_get(1);

    /* Step 2: Stage name+ELF path and call SpawnServer */
    stage_spawn_config(manifest.name, manifest.elf_path);

    microkit_mr_set(0, OP_SPAWN_LAUNCH);
    microkit_mr_set(1, manifest.cap_classes);
    microkit_mr_set(2, slot->vnic_id);
    microkit_ppcall(APP_CH_SPAWN, microkit_msginfo_new(OP_SPAWN_LAUNCH, 3));
    uint32_t spawn_rc = (uint32_t)microkit_mr_get(0);

    if (spawn_rc != SPAWN_OK) {
        /* Tear down vNIC on failure */
        if (slot->vnic_id != 0xFFFFFFFFu) {
            microkit_mr_set(0, OP_NET_VNIC_DESTROY);
            microkit_mr_set(1, slot->vnic_id);
            microkit_ppcall(APP_CH_NET,
                            microkit_msginfo_new(OP_NET_VNIC_DESTROY, 2));
        }
        slot->active = false;
        return microkit_msginfo_new(APP_ERR_SPAWN, 0);
    }

    slot->spawn_slot = (uint32_t)microkit_mr_get(1);  /* assigned app_id from spawn */

    /* Step 3: Register HTTP route (non-fatal if no prefix or http_svc absent) */
    if (manifest.http_prefix[0] != '\0') {
        uint32_t plen = s_len(manifest.http_prefix);
        microkit_mr_set(0, OP_HTTP_REGISTER);
        microkit_mr_set(1, slot->app_id);
        microkit_mr_set(2, slot->vnic_id);
        microkit_mr_set(3, plen);
        pack_prefix(manifest.http_prefix, plen);
        microkit_ppcall(APP_CH_HTTP,
                        microkit_msginfo_new(OP_HTTP_REGISTER,
                                            HTTP_PREFIX_MR_BASE +
                                            HTTP_PREFIX_MR_COUNT));
    }

    slot->active = true;
    slot->state  = APP_STATE_RUNNING;
    LOG("app launched");

    microkit_mr_set(0, APP_OK);
    microkit_mr_set(1, slot->app_id);
    microkit_mr_set(2, slot->vnic_id);
    return microkit_msginfo_new(APP_OK, 3);
}

/* ── OP_APP_KILL ─────────────────────────────────────────────────────────── */
static microkit_msginfo handle_kill(void)
{
    uint32_t app_id = (uint32_t)microkit_mr_get(1);
    app_entry_t *entry = find_by_id(app_id);
    if (!entry) return microkit_msginfo_new(APP_ERR_NOT_FOUND, 0);

    entry->state = APP_STATE_STOPPING;

    /* Kill spawn slot */
    microkit_mr_set(0, OP_SPAWN_KILL);
    microkit_mr_set(1, app_id);
    microkit_ppcall(APP_CH_SPAWN, microkit_msginfo_new(OP_SPAWN_KILL, 2));

    /* Destroy vNIC */
    if (entry->vnic_id != 0xFFFFFFFFu) {
        microkit_mr_set(0, OP_NET_VNIC_DESTROY);
        microkit_mr_set(1, entry->vnic_id);
        microkit_ppcall(APP_CH_NET,
                        microkit_msginfo_new(OP_NET_VNIC_DESTROY, 2));
    }

    /* Unregister HTTP route (use app_id to identify; http_svc resolves) */
    if (entry->http_prefix[0] != '\0') {
        /* http_svc OP_HTTP_UNREGISTER takes handler_id; pass app_id as hint */
        microkit_mr_set(0, OP_HTTP_UNREGISTER);
        microkit_mr_set(1, app_id);
        microkit_ppcall(APP_CH_HTTP,
                        microkit_msginfo_new(OP_HTTP_UNREGISTER, 2));
    }

    entry->active = false;
    return microkit_msginfo_new(APP_OK, 0);
}

/* ── OP_APP_STATUS ───────────────────────────────────────────────────────── */
static microkit_msginfo handle_status(void)
{
    uint32_t app_id = (uint32_t)microkit_mr_get(1);
    app_entry_t *entry = find_by_id(app_id);
    if (!entry) return microkit_msginfo_new(APP_ERR_NOT_FOUND, 0);

    microkit_mr_set(0, APP_OK);
    microkit_mr_set(1, entry->state);
    microkit_mr_set(2, entry->vnic_id);
    microkit_mr_set(3, entry->spawn_slot);
    return microkit_msginfo_new(APP_OK, 4);
}

/* ── OP_APP_LIST ─────────────────────────────────────────────────────────── */
static microkit_msginfo handle_list(void)
{
    if (!app_manifest_shmem_vaddr)
        return microkit_msginfo_new(APP_ERR_INVAL, 0);

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
        /* copy name */
        for (uint32_t j = 0; j < APP_MANIFEST_NAME_MAX; j++)
            e->name[j] = apps[i].name[j];
        /* copy http_prefix */
        for (uint32_t j = 0; j < APP_MANIFEST_PREFIX_MAX; j++)
            e->http_prefix[j] = apps[i].http_prefix[j];
    }

    microkit_mr_set(0, APP_OK);
    microkit_mr_set(1, count);
    return microkit_msginfo_new(APP_OK, 2);
}

/* ── OP_APP_HEALTH ───────────────────────────────────────────────────────── */
static microkit_msginfo handle_health(void)
{
    microkit_mr_set(0, APP_OK);
    microkit_mr_set(1, count_running());
    microkit_mr_set(2, APP_MANAGER_VERSION);
    return microkit_msginfo_new(APP_OK, 3);
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    for (uint32_t i = 0; i < APP_MAX_APPS; i++) apps[i].active = false;
    LOG("init complete");
}

void notified(microkit_channel ch)
{
    (void)ch;
    /* no inbound notifications in the current design */
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    if (ch != APP_CH_CONTROLLER)
        return microkit_msginfo_new(APP_ERR_INVAL, 0);

    uint32_t op = (uint32_t)microkit_mr_get(0);
    (void)msginfo;

    switch (op) {
    case OP_APP_LAUNCH:  return handle_launch();
    case OP_APP_KILL:    return handle_kill();
    case OP_APP_STATUS:  return handle_status();
    case OP_APP_LIST:    return handle_list();
    case OP_APP_HEALTH:  return handle_health();
    default:
        return microkit_msginfo_new(APP_ERR_INVAL, 0);
    }
}
