/*
 * http_svc.c — HTTP Service Protection Domain
 *
 * Active PD (priority 140, console slot 19).
 *
 * Maintains a table of up to HTTP_MAX_HANDLERS URL-prefix → app_id route
 * mappings.  AppManager registers routes here when launching apps and
 * unregisters them on teardown.  The Rust http-gateway
 * (userspace/servers/http-gateway/) calls OP_HTTP_DISPATCH to find which
 * app should handle an incoming request.
 *
 * URL prefixes are exchanged entirely in message registers (8 bytes per MR,
 * MR4..MR11) to avoid requiring a shared memory region between callers and
 * this PD.  OP_HTTP_LIST writes the full handler table to http_req_shmem.
 *
 * Channel layout (from this PD's perspective):
 *   HTTP_CH_CONTROLLER   (0): controller PPCs in (pp=true)
 *   HTTP_CH_APP_MANAGER  (1): app_manager PPCs in (pp=true)
 */

#include <microkit.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "agentos.h"
#include "contracts/http_svc_contract.h"
#include "http_svc.h"

/* ── Shared memory globals (set by Microkit via setvar_vaddr) ────────────── */
uintptr_t http_req_shmem_vaddr;

/* Weak console_rings fallback */
uintptr_t log_drain_rings_vaddr;

#define LOG(msg) log_drain_write(HTTP_SVC_CONSOLE_SLOT, HTTP_SVC_PD_ID, \
                             "[http_svc] " msg "\n")

/* ── Handler table ───────────────────────────────────────────────────────── */
typedef struct {
    bool     active;
    uint32_t handler_id;
    uint32_t app_id;
    uint32_t vnic_id;
    char     prefix[HTTP_PREFIX_MAX];
    uint32_t prefix_len;
} handler_entry_t;

static handler_entry_t handlers[HTTP_MAX_HANDLERS];
static uint32_t        next_handler_id = 0;

/* ── String helpers (no libc) ────────────────────────────────────────────── */

static uint32_t s_len(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void s_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i + 1 < max && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ── MR prefix unpacking ─────────────────────────────────────────────────── */
/* Prefix is in MR4..MR11 as defined by HTTP_PREFIX_MR_BASE / _COUNT */
static void unpack_prefix(char *dst, uint32_t len)
{
    uint32_t cap = len < HTTP_PREFIX_MAX ? len : HTTP_PREFIX_MAX - 1;
    for (uint32_t w = 0; w < HTTP_PREFIX_MR_COUNT; w++) {
        uint64_t word = (uint64_t)microkit_mr_get(HTTP_PREFIX_MR_BASE + w);
        for (uint32_t b = 0; b < 8; b++) {
            uint32_t idx = w * 8u + b;
            if (idx < cap)
                dst[idx] = (char)(word >> (b * 8u));
        }
    }
    dst[cap] = '\0';
}

/* ── MR prefix packing for OP_HTTP_DISPATCH caller-side pattern ─────────── */
/* http_svc itself receives prefixes already packed in MRs from the caller */

/* ── Table helpers ───────────────────────────────────────────────────────── */

static handler_entry_t *alloc_handler(void)
{
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++)
        if (!handlers[i].active) return &handlers[i];
    return NULL;
}

static handler_entry_t *find_by_handler_id(uint32_t hid)
{
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++)
        if (handlers[i].active && handlers[i].handler_id == hid) return &handlers[i];
    return NULL;
}

static handler_entry_t *find_by_app_id(uint32_t app_id)
{
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++)
        if (handlers[i].active && handlers[i].app_id == app_id) return &handlers[i];
    return NULL;
}

static uint32_t count_active(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++)
        n += handlers[i].active ? 1u : 0u;
    return n;
}

/*
 * longest_prefix_match — scan the handler table for the entry whose prefix
 * is the longest that is a prefix of the given URL path.
 * Returns NULL if no match.
 */
static handler_entry_t *longest_prefix_match(const char *path, uint32_t path_len)
{
    handler_entry_t *best = NULL;
    uint32_t         best_len = 0;

    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++) {
        if (!handlers[i].active) continue;
        uint32_t plen = handlers[i].prefix_len;
        if (plen > path_len || plen < best_len) continue;

        bool match = true;
        for (uint32_t j = 0; j < plen; j++) {
            if (handlers[i].prefix[j] != path[j]) { match = false; break; }
        }
        if (match) { best = &handlers[i]; best_len = plen; }
    }
    return best;
}

/* ── OP_HTTP_REGISTER ────────────────────────────────────────────────────── */
static microkit_msginfo handle_register(void)
{
    uint32_t app_id    = (uint32_t)microkit_mr_get(1);
    uint32_t vnic_id   = (uint32_t)microkit_mr_get(2);
    uint32_t prefix_len = (uint32_t)microkit_mr_get(3);

    if (prefix_len == 0 || prefix_len > HTTP_PREFIX_MAX - 1)
        return microkit_msginfo_new(HTTP_ERR_INVAL, 0);

    handler_entry_t *h = alloc_handler();
    if (!h) return microkit_msginfo_new(HTTP_ERR_NO_SLOTS, 0);

    unpack_prefix(h->prefix, prefix_len);
    h->prefix_len  = prefix_len;
    h->app_id      = app_id;
    h->vnic_id     = vnic_id;
    h->handler_id  = next_handler_id++;
    h->active      = true;

    LOG("route registered");

    microkit_mr_set(0, HTTP_OK);
    microkit_mr_set(1, h->handler_id);
    return microkit_msginfo_new(HTTP_OK, 1);
}

/* ── OP_HTTP_UNREGISTER ──────────────────────────────────────────────────── */
static microkit_msginfo handle_unregister(void)
{
    uint32_t id = (uint32_t)microkit_mr_get(1);

    /* Try by handler_id first, then fall back to app_id lookup */
    handler_entry_t *h = find_by_handler_id(id);
    if (!h) h = find_by_app_id(id);
    if (!h) return microkit_msginfo_new(HTTP_ERR_NOT_FOUND, 0);

    h->active = false;
    return microkit_msginfo_new(HTTP_OK, 0);
}

/* ── OP_HTTP_DISPATCH ────────────────────────────────────────────────────── */
/*
 * Caller packs the URL path into MR1..MR8 (8 bytes per MR, 64 bytes total).
 * We do a longest-prefix match and return the owning app_id.
 */
static microkit_msginfo handle_dispatch(void)
{
    char path[65];
    path[64] = '\0';
    for (uint32_t w = 0; w < 8; w++) {
        uint64_t word = (uint64_t)microkit_mr_get(1 + w);
        for (uint32_t b = 0; b < 8; b++) {
            uint32_t idx = w * 8u + b;
            if (idx < 64)
                path[idx] = (char)(word >> (b * 8u));
        }
    }

    /* Find null terminator to determine actual path length */
    uint32_t path_len = 0;
    while (path_len < 64 && path[path_len]) path_len++;

    handler_entry_t *h = longest_prefix_match(path, path_len);

    microkit_mr_set(0, HTTP_OK);
    microkit_mr_set(1, h ? h->app_id    : HTTP_APP_ID_NONE);
    microkit_mr_set(2, h ? h->vnic_id   : 0xFFFFFFFFu);
    microkit_mr_set(3, h ? h->handler_id : 0xFFFFFFFFu);
    return microkit_msginfo_new(HTTP_OK, 3);
}

/* ── OP_HTTP_LIST ────────────────────────────────────────────────────────── */
static microkit_msginfo handle_list(void)
{
    if (!http_req_shmem_vaddr)
        return microkit_msginfo_new(HTTP_ERR_INVAL, 0);

    volatile http_handler_entry_t *out =
        (volatile http_handler_entry_t *)http_req_shmem_vaddr;

    uint32_t count = 0;
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++) {
        if (!handlers[i].active) continue;
        volatile http_handler_entry_t *e = &out[count++];
        e->active     = true;
        e->_pad[0]    = 0; e->_pad[1] = 0; e->_pad[2] = 0;
        e->handler_id = handlers[i].handler_id;
        e->app_id     = handlers[i].app_id;
        e->vnic_id    = handlers[i].vnic_id;
        for (uint32_t j = 0; j < HTTP_PREFIX_MAX; j++)
            e->prefix[j] = handlers[i].prefix[j];
    }

    microkit_mr_set(0, HTTP_OK);
    microkit_mr_set(1, count);
    return microkit_msginfo_new(HTTP_OK, 1);
}

/* ── OP_HTTP_HEALTH ──────────────────────────────────────────────────────── */
static microkit_msginfo handle_health(void)
{
    microkit_mr_set(0, HTTP_OK);
    microkit_mr_set(1, count_active());
    microkit_mr_set(2, HTTP_SVC_VERSION);
    return microkit_msginfo_new(HTTP_OK, 2);
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    for (uint32_t i = 0; i < HTTP_MAX_HANDLERS; i++) handlers[i].active = false;
    LOG("init complete");
}

void notified(microkit_channel ch)
{
    (void)ch;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)msginfo;
    uint32_t op = (uint32_t)microkit_mr_get(0);

    /* Both channels accept the same opcode set */
    if (ch != HTTP_CH_CONTROLLER && ch != HTTP_CH_APP_MANAGER)
        return microkit_msginfo_new(HTTP_ERR_INVAL, 0);

    switch (op) {
    case OP_HTTP_REGISTER:   return handle_register();
    case OP_HTTP_UNREGISTER: return handle_unregister();
    case OP_HTTP_DISPATCH:   return handle_dispatch();
    case OP_HTTP_LIST:       return handle_list();
    case OP_HTTP_HEALTH:     return handle_health();
    default:
        return microkit_msginfo_new(HTTP_ERR_INVAL, 0);
    }
}
