/*
 * sel4_client.h — client-side nameserver lookup and service call helpers
 *
 * Provides the standard client pattern for agentOS protection domains that
 * need to call other services at runtime:
 *
 *   1. At PD startup, call sel4_client_init() with the nameserver endpoint
 *      and a CNode slot range to store minted endpoint capabilities.
 *   2. Before calling a service, call sel4_client_connect() by name.
 *      The result (an seL4 endpoint cap) is cached so subsequent lookups
 *      are a local array scan with no IPC.
 *   3. Issue RPCs with sel4_client_call().
 *
 * All functions are static inline — no separate compilation unit needed.
 * No libc dependency.  Safe for -nostdinc -ffreestanding builds.
 *
 * Example:
 *
 *   static sel4_client_t client;
 *
 *   void pd_init(void) {
 *       sel4_client_init(&client, NS_EP_CAP, MY_CNODE_CAP, FIRST_FREE_SLOT);
 *   }
 *
 *   void do_work(void) {
 *       seL4_CPtr ep;
 *       if (sel4_client_connect(&client, "agentfs", &ep) != SEL4_ERR_OK)
 *           return;
 *       sel4_msg_t rep;
 *       sel4_client_call(ep, OP_AGENTFS_READ, &req_data, sizeof(req_data), &rep);
 *   }
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "sel4_ipc.h"
#include "nameserver.h"  /* OP_NS_LOOKUP, NS_NAME_MAX, NS_OK, NS_ERR_NOT_FOUND */

/* ── Cache geometry ──────────────────────────────────────────────────────── */

/* Maximum number of simultaneously cached service connections per PD. */
#define SEL4_CLIENT_CACHE_SIZE  16u

/*
 * Maximum length of a service name (including NUL terminator) stored in the
 * cache.  Names longer than this are silently truncated for cache comparison
 * but still sent in full to the nameserver.
 */
#define SEL4_CLIENT_NAME_MAX    48u

/* ── Cache entry ─────────────────────────────────────────────────────────── */

/*
 * sel4_client_entry_t — one slot in the connection cache.
 *
 * Fields:
 *   name[]  — NUL-terminated service name (truncated at SEL4_CLIENT_NAME_MAX-1)
 *   ep      — seL4 endpoint capability for this service
 *   valid   — 1 if this slot is populated, 0 if free
 */
typedef struct {
    char      name[SEL4_CLIENT_NAME_MAX];
    seL4_CPtr ep;
    uint32_t  valid;
} sel4_client_entry_t;

/* ── Client state ────────────────────────────────────────────────────────── */

/*
 * sel4_client_t — per-PD client connection cache.
 *
 * Typically declared as a static global in the PD's main compilation unit.
 * Stack allocation is safe if the lifetime covers all sel4_client_call()
 * invocations.
 *
 * Fields:
 *   entries[]        — fixed-size connection cache
 *   nameserver_ep    — well-known nameserver endpoint cap (from root task boot)
 *   my_cnode         — this PD's CNode capability
 *   next_free_slot   — next CNode slot available for storing minted ep caps
 */
typedef struct {
    sel4_client_entry_t entries[SEL4_CLIENT_CACHE_SIZE];
    seL4_CPtr           nameserver_ep;
    seL4_CPtr           my_cnode;
    seL4_Word           next_free_slot;
} sel4_client_t;

/* ── String helpers (no libc) ────────────────────────────────────────────── */

/*
 * sel4_streq() — compare two strings byte-by-byte up to `max` characters.
 *
 * Returns 1 if the strings are equal within the first `max` bytes (or both
 * NUL-terminate at the same position before that), 0 otherwise.
 *
 * No libc dependency.  Suitable for -nostdinc -ffreestanding builds.
 */
static inline int sel4_streq(const char *a, const char *b, uint32_t max)
{
    for (uint32_t i = 0; i < max; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;  /* both NUL at same position */
    }
    return 1;  /* equal for all `max` characters */
}

/*
 * _sel4_copy_name() — copy a service name into a fixed buffer with NUL
 *                     termination.  No libc.  Truncates at (dst_max - 1).
 */
static inline void _sel4_copy_name(char *dst, const char *src, uint32_t dst_max)
{
    uint32_t i;
    for (i = 0; i + 1u < dst_max && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ── Initialisation ──────────────────────────────────────────────────────
 *
 * sel4_client_init() — prepare the client cache for use.
 *
 * Parameters:
 *   client           — client state to initialise
 *   nameserver_ep    — endpoint cap for the nameserver PD
 *   my_cnode         — this PD's CNode cap (for storing minted endpoint caps)
 *   first_free_slot  — first CNode slot the client may use to store ep caps
 *                      returned by the nameserver; advances monotonically as
 *                      services are connected.
 */
static inline void sel4_client_init(sel4_client_t *client,
                                     seL4_CPtr nameserver_ep,
                                     seL4_CPtr my_cnode,
                                     seL4_Word first_free_slot)
{
    for (uint32_t i = 0; i < SEL4_CLIENT_CACHE_SIZE; i++) {
        client->entries[i].valid     = 0u;
        client->entries[i].ep        = 0u;
        client->entries[i].name[0]   = '\0';
    }
    client->nameserver_ep  = nameserver_ep;
    client->my_cnode       = my_cnode;
    client->next_free_slot = first_free_slot;
}

/* ── sel4_client_call ────────────────────────────────────────────────────
 *
 * Call a service endpoint with a request, wait for reply.
 *
 * Builds a sel4_msg_t with the given opcode and copies up to
 * SEL4_MSG_DATA_BYTES bytes of payload into msg.data (truncated silently
 * if payload_len is larger).  Calls sel4_call() and returns rep->opcode.
 *
 * Parameters:
 *   ep          — service endpoint cap (from sel4_client_connect)
 *   opcode      — operation selector to place in msg.opcode
 *   payload     — pointer to request payload bytes (may be NULL)
 *   payload_len — number of bytes to copy from payload
 *   rep         — caller-supplied buffer for the reply message
 *
 * Returns: rep->opcode (SEL4_ERR_OK on success, SEL4_ERR_* on error).
 */
static inline uint32_t sel4_client_call(seL4_CPtr ep,
                                         uint32_t opcode,
                                         const void *payload,
                                         uint32_t payload_len,
                                         sel4_msg_t *rep)
{
    sel4_msg_t req;
    req.opcode = opcode;

    /* Clamp payload length to message data capacity. */
    uint32_t copy_len = payload_len < SEL4_MSG_DATA_BYTES
                        ? payload_len : SEL4_MSG_DATA_BYTES;
    req.length = copy_len;

    /* Copy payload bytes (no libc memcpy). */
    if (payload && copy_len > 0u) {
        const uint8_t *src = (const uint8_t *)payload;
        for (uint32_t i = 0; i < copy_len; i++)
            req.data[i] = src[i];
    }
    /* Zero remainder to avoid leaking stack bytes to the server. */
    for (uint32_t i = copy_len; i < SEL4_MSG_DATA_BYTES; i++)
        req.data[i] = 0u;

    sel4_call(ep, &req, rep);
    return rep->opcode;
}

/* ── sel4_client_connect ─────────────────────────────────────────────────
 *
 * Look up a service by name via the nameserver.  Caches the result so that
 * subsequent calls for the same name return immediately without IPC.
 *
 * Lookup protocol:
 *   - Build a sel4_msg_t with opcode = OP_NS_LOOKUP and the service name
 *     packed into the first min(NS_SERVICE_NAME_MAX-1, 47) bytes of data[].
 *   - Call sel4_call(client->nameserver_ep, &req, &rep).
 *   - If rep.opcode == SEL4_ERR_OK (nameserver maps NS_OK → SEL4_ERR_OK):
 *       the nameserver has minted a copy of the service's endpoint cap into
 *       client->next_free_slot of this PD's CNode.  Record ep = next_free_slot,
 *       advance next_free_slot, add to cache.
 *   - Otherwise propagate the error code.
 *
 * Parameters:
 *   client   — initialised client state
 *   name     — NUL-terminated service name (e.g. "agentfs")
 *   ep_out   — receives the endpoint cap on SEL4_ERR_OK
 *
 * Returns: SEL4_ERR_OK on success, SEL4_ERR_NOT_FOUND if the nameserver
 *          does not know the service, or another SEL4_ERR_* on failure.
 *
 * Cache behaviour:
 *   O(n) scan where n <= SEL4_CLIENT_CACHE_SIZE (16).  Cache is never
 *   evicted; if all 16 slots are full, connect fails with SEL4_ERR_NO_MEM.
 *
 * NOTE on nameserver reply mapping:
 *   The nameserver returns NS_OK (0) on success and NS_ERR_NOT_FOUND on
 *   failure.  Because SEL4_ERR_OK == 0 and SEL4_ERR_NOT_FOUND == 2 we map:
 *     NS_OK           → SEL4_ERR_OK
 *     NS_ERR_NOT_FOUND → SEL4_ERR_NOT_FOUND
 *     any other NS_ERR_* → SEL4_ERR_INTERNAL
 */
static inline uint32_t sel4_client_connect(sel4_client_t *client,
                                            const char *name,
                                            seL4_CPtr *ep_out)
{
    /* ── 1. Cache lookup ─────────────────────────────────────────────────── */
    for (uint32_t i = 0; i < SEL4_CLIENT_CACHE_SIZE; i++) {
        if (client->entries[i].valid &&
            sel4_streq(client->entries[i].name, name, SEL4_CLIENT_NAME_MAX)) {
            *ep_out = client->entries[i].ep;
            return SEL4_ERR_OK;
        }
    }

    /* ── 2. Find a free cache slot (before going to the nameserver) ──────── */
    uint32_t free_slot = SEL4_CLIENT_CACHE_SIZE;  /* sentinel = not found */
    for (uint32_t i = 0; i < SEL4_CLIENT_CACHE_SIZE; i++) {
        if (!client->entries[i].valid) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == SEL4_CLIENT_CACHE_SIZE)
        return SEL4_ERR_NO_MEM;  /* cache full */

    /* ── 3. Nameserver IPC ───────────────────────────────────────────────── */
    /*
     * Pack the lookup request into sel4_msg_t.data[].
     * Layout of data[]:
     *   data[0..N-1] = NUL-terminated service name (truncated at 47 bytes)
     *   data[N]      = '\0'  (explicit NUL terminator within the 48-byte field)
     *
     * The nameserver on the far side reads the name directly from req.data
     * after unpacking the MRs.  opcode = OP_NS_LOOKUP in req.opcode.
     *
     * NS_SERVICE_NAME_MAX is not defined in this header; we use NS_NAME_MAX
     * (32) from nameserver.h.  We cap at SEL4_MSG_DATA_BYTES - 1 = 47 chars
     * so that a NUL terminator always fits within data[].
     */
#define _SEL4_NS_NAME_COPY_MAX  (SEL4_MSG_DATA_BYTES - 1u)

    sel4_msg_t req;
    req.opcode = (uint32_t)OP_NS_LOOKUP;

    /* Copy name into data[], NUL-terminate, zero remainder. */
    uint32_t ni = 0u;
    for (; ni < _SEL4_NS_NAME_COPY_MAX && name[ni] != '\0'; ni++)
        req.data[ni] = (uint8_t)name[ni];
    req.data[ni] = '\0';
    for (uint32_t j = ni + 1u; j < SEL4_MSG_DATA_BYTES; j++)
        req.data[j] = 0u;
    req.length = ni;  /* byte count, excluding NUL */

    sel4_msg_t rep;
    sel4_call(client->nameserver_ep, &req, &rep);

    /* ── 4. Map nameserver reply codes to SEL4_ERR_* ─────────────────────── */
    uint32_t ns_status = rep.opcode;
    if (ns_status == (uint32_t)NS_OK) {
        /*
         * The nameserver has placed the minted endpoint cap into
         * client->next_free_slot in our CNode.  Record and advance.
         */
        seL4_CPtr ep = client->next_free_slot;
        client->next_free_slot++;

        /* Populate cache entry. */
        _sel4_copy_name(client->entries[free_slot].name, name,
                        SEL4_CLIENT_NAME_MAX);
        client->entries[free_slot].ep    = ep;
        client->entries[free_slot].valid = 1u;

        *ep_out = ep;
        return SEL4_ERR_OK;
    }

    if (ns_status == (uint32_t)NS_ERR_NOT_FOUND)
        return SEL4_ERR_NOT_FOUND;

    /* All other nameserver errors map to SEL4_ERR_INTERNAL. */
    return SEL4_ERR_INTERNAL;

#undef _SEL4_NS_NAME_COPY_MAX
}
