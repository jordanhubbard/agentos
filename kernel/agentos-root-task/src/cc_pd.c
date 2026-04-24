/*
 * cc_pd.c — agentOS Command-and-Control Protection Domain
 *
 * Pure IPC relay and multiplexer.  Receives MSG_CC_* from external callers
 * (agentctl, mesh agents) and routes each call to the appropriate service PD:
 *
 *   MSG_CC_LIST_GUESTS        → vibe_engine   (MSG_VIBEOS_LIST)
 *   MSG_CC_LIST_DEVICES       → device PD     (selected by dev_type)
 *   MSG_CC_LIST_POLECATS      → agent_pool    (MSG_AGENTPOOL_STATUS)
 *   MSG_CC_GUEST_STATUS       → vibe_engine   (MSG_VIBEOS_STATUS)
 *   MSG_CC_DEVICE_STATUS      → device PD     (selected by dev_type)
 *   MSG_CC_ATTACH_FRAMEBUFFER → framebuffer_pd (MSG_FB_FLIP handle probe)
 *   MSG_CC_SEND_INPUT         → guest_pd      (MSG_GUEST_SEND_INPUT)
 *   MSG_CC_SNAPSHOT           → vibe_engine   (MSG_VIBEOS_SNAPSHOT)
 *   MSG_CC_RESTORE            → vibe_engine   (MSG_VIBEOS_RESTORE)
 *   MSG_CC_LOG_STREAM         → log_drain     (OP_LOG_WRITE)
 *
 * Session management (MSG_CC_CONNECT / DISCONNECT / SEND / RECV / STATUS /
 * LIST) is also handled here — sessions track external callers.
 *
 * INVARIANT: cc_pd contains ZERO policy.  It is a relay only.
 * No routing logic beyond opcode dispatch and dev_type field lives here.
 * No command may be implemented here that belongs in a service PD.
 *
 * Channel assignments (outbound from cc_pd):
 *   CH_CC_OUT_VIBE    — cc_pd → vibe_engine   (MSG_VIBEOS_*)
 *   CH_CC_OUT_GUEST   — cc_pd → guest_pd      (MSG_GUEST_*)
 *   CH_CC_OUT_FB      — cc_pd → framebuffer_pd (MSG_FB_*)
 *   CH_CC_OUT_SERIAL  — cc_pd → serial_pd     (MSG_SERIAL_STATUS)
 *   CH_CC_OUT_NET     — cc_pd → net_pd        (MSG_NET_DEV_STATUS)
 *   CH_CC_OUT_BLOCK   — cc_pd → block_pd      (MSG_BLOCK_STATUS)
 *   CH_CC_OUT_USB     — cc_pd → usb_pd        (MSG_USB_LIST)
 *   CH_CC_OUT_POOL    — cc_pd → agent_pool    (MSG_AGENTPOOL_STATUS)
 *   CH_CC_OUT_LOG     — cc_pd → log_drain     (OP_LOG_WRITE)
 *
 * Shared memory (setvar_vaddr):
 *   cc_shmem_vaddr    — caller-facing shmem (requests + responses)
 *   cc_vibe_shmem_vaddr  — cc_pd ↔ vibe_engine shmem
 *   cc_guest_shmem_vaddr — cc_pd ↔ guest_pd shmem
 *   cc_fb_shmem_vaddr    — cc_pd ↔ framebuffer_pd shmem
 *   cc_dev_shmem_vaddr   — cc_pd ↔ device PDs shmem (serial/net/block/usb)
 *
 * Channel: CH_CC_PD (see agentos.h) — inbound PPC from callers
 * Priority: 160
 * Mode: passive (woken by PPC from callers)
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/vibeos_contract.h"
#include "contracts/cc_contract.h"
#include "contracts/guest_contract.h"
#include "contracts/framebuffer_contract.h"
#include "contracts/log_drain_contract.h"
#include "contracts/agent_pool_contract.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ─── Outbound channel assignments ───────────────────────────────────────── */

#define CH_CC_OUT_VIBE    0u   /* cc_pd → vibe_engine (MSG_VIBEOS_*) */
#define CH_CC_OUT_GUEST   1u   /* cc_pd → guest_pd (MSG_GUEST_*) */
#define CH_CC_OUT_FB      2u   /* cc_pd → framebuffer_pd (MSG_FB_*) */
#define CH_CC_OUT_SERIAL  3u   /* cc_pd → serial_pd (MSG_SERIAL_STATUS) */
#define CH_CC_OUT_NET     4u   /* cc_pd → net_pd (MSG_NET_DEV_STATUS) */
#define CH_CC_OUT_BLOCK   5u   /* cc_pd → block_pd (MSG_BLOCK_STATUS) */
#define CH_CC_OUT_USB     6u   /* cc_pd → usb_pd (MSG_USB_LIST) */
#define CH_CC_OUT_POOL    7u   /* cc_pd → agent_pool (MSG_AGENTPOOL_STATUS) */
#define CH_CC_OUT_LOG     8u   /* cc_pd → log_drain (OP_LOG_WRITE) */

/* ─── Shmem (Microkit setvar_vaddr) ─────────────────────────────────────── */

uintptr_t cc_shmem_vaddr;        /* caller-facing shmem */
uintptr_t cc_vibe_shmem_vaddr;   /* cc_pd ↔ vibe_engine */
uintptr_t cc_guest_shmem_vaddr;  /* cc_pd ↔ guest_pd */
uintptr_t cc_fb_shmem_vaddr;     /* cc_pd ↔ framebuffer_pd */
uintptr_t cc_dev_shmem_vaddr;    /* cc_pd ↔ device PDs (shared region) */

uintptr_t log_drain_rings_vaddr; /* required by log_drain_write() helper */

/* ─── Session table ──────────────────────────────────────────────────────── */

typedef struct {
    bool             active;
    uint32_t owner;
    uint32_t         client_badge;
    uint32_t         state;            /* CC_SESSION_STATE_* */
    uint32_t         ticks_since_active;
} cc_session_t;

static cc_session_t sessions[CC_MAX_SESSIONS];

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static int alloc_session(void)
{
    for (int i = 0; i < (int)CC_MAX_SESSIONS; i++) {
        if (!sessions[i].active)
            return i;
    }
    return -1;
}

static bool valid_session(uint32_t sid, uint32_t ch)
{
    return sid < CC_MAX_SESSIONS &&
           sessions[sid].active &&
           sessions[sid].owner == ch &&
           sessions[sid].state != CC_SESSION_STATE_EXPIRED;
}

static void touch_session(uint32_t sid)
{
    if (sid < CC_MAX_SESSIONS && sessions[sid].active)
        sessions[sid].ticks_since_active = 0;
}

/* ─── Session management handlers ───────────────────────────────────────── */

static void handle_connect(uint32_t ch)
{
    uint32_t badge = (uint32_t)msg_u32(req, 4);
    int s = alloc_session();
    if (s < 0) {
        rep_u32(rep, 0, CC_ERR_NO_SESSIONS);
        rep_u32(rep, 4, 0u);
        return;
    }
    sessions[s].active       = true;
    sessions[s].owner        = ch;
    sessions[s].client_badge = badge;
    sessions[s].state        = CC_SESSION_STATE_CONNECTED;
    sessions[s].ticks_since_active = 0;

    rep_u32(rep, 0, CC_OK);
    rep_u32(rep, 4, (uint32_t)s);
}

static void handle_disconnect(uint32_t ch)
{
    uint32_t sid = (uint32_t)msg_u32(req, 4);
    if (!valid_session(sid, ch)) {
        rep_u32(rep, 0, CC_ERR_BAD_SESSION);
        return;
    }
    sessions[sid].active = false;
    rep_u32(rep, 0, CC_OK);
}

static void handle_status(uint32_t ch)
{
    uint32_t sid = (uint32_t)msg_u32(req, 4);
    if (!valid_session(sid, ch)) {
        rep_u32(rep, 0, CC_ERR_BAD_SESSION);
        rep_u32(rep, 4, 0u);
        rep_u32(rep, 8, 0u);
        rep_u32(rep, 12, 0u);
        return;
    }
    rep_u32(rep, 0, CC_OK);
    rep_u32(rep, 4, sessions[sid].state);
    rep_u32(rep, 8, 0u);                        /* no pending responses in relay model */
    rep_u32(rep, 12, sessions[sid].ticks_since_active);
}

static void handle_list_sessions(void)
{
    cc_session_info_t *out = (cc_session_info_t *)cc_shmem_vaddr;
    uint32_t count = 0;

    for (uint32_t i = 0; i < CC_MAX_SESSIONS; i++) {
        if (sessions[i].active) {
            out[count].session_id        = i;
            out[count].state             = sessions[i].state;
            out[count].client_badge      = sessions[i].client_badge;
            out[count].ticks_since_active = sessions[i].ticks_since_active;
            count++;
        }
    }
    rep_u32(rep, 0, count);
}

/* ─── Relay helpers ──────────────────────────────────────────────────────── */

/*
 * relay_vibe: forward a MSG_VIBEOS_* call to vibe_engine.
 * MRs must be set by caller before invoking.  Returns the raw MR0 result.
 */
static uint32_t relay_vibe(uint32_t opcode, uint32_t mr_count)
{
    rep_u32(rep, 0, opcode);
    /* E5-S8: ppcall stubbed */
    return (uint32_t)msg_u32(req, 0);
}

/*
 * relay_guest: forward a MSG_GUEST_* call to guest_pd.
 */
static uint32_t relay_guest(uint32_t opcode, uint32_t mr_count)
{
    rep_u32(rep, 0, opcode);
    /* E5-S8: ppcall stubbed */
    return (uint32_t)msg_u32(req, 0);
}

/* ─── Direct relay handlers (Phase 5a) ──────────────────────────────────── */

/*
 * MSG_CC_LIST_GUESTS → MSG_VIBEOS_LIST
 * Relays to vibe_engine; result (vibeos_info_t[] in cc_vibe_shmem) is
 * copied to cc_shmem as cc_guest_info_t[].
 */
static void handle_list_guests(void)
{
    rep_u32(rep, 0, MSG_VIBEOS_LIST);
    /* E5-S8: ppcall stubbed */
    uint32_t count = (uint32_t)msg_u32(req, 0);

    /* Copy cc_vibe_shmem entries to caller's cc_shmem as cc_guest_info_t.
     * vibeos_info_t layout: handle, os_type, state, ram_mb, uptime_ticks, node_id */
    const vibeos_info_t *src  = (const vibeos_info_t *)cc_vibe_shmem_vaddr;
    cc_guest_info_t *dst      = (cc_guest_info_t *)cc_shmem_vaddr;

    for (uint32_t i = 0; i < count; i++) {
        dst[i].guest_handle = src[i].handle;
        dst[i].state        = src[i].state;
        dst[i].os_type      = src[i].os_type;
        dst[i].arch         = 0u;  /* vibeos_info_t has no arch field */
    }

    rep_u32(rep, 0, count);
}

/*
 * MSG_CC_LIST_DEVICES → device PD selected by dev_type in MR1.
 * Routes:
 *   CC_DEV_TYPE_SERIAL → MSG_SERIAL_STATUS on CH_CC_OUT_SERIAL
 *   CC_DEV_TYPE_NET    → MSG_NET_DEV_STATUS on CH_CC_OUT_NET
 *   CC_DEV_TYPE_BLOCK  → MSG_BLOCK_STATUS on CH_CC_OUT_BLOCK
 *   CC_DEV_TYPE_USB    → MSG_USB_LIST on CH_CC_OUT_USB
 *   CC_DEV_TYPE_FB     → MSG_FB_* (handled via attach path)
 */
static void handle_list_devices(void)
{
    uint32_t dev_type = (uint32_t)msg_u32(req, 4);
    cc_device_info_t *out = (cc_device_info_t *)cc_shmem_vaddr;

    switch (dev_type) {
    case CC_DEV_TYPE_SERIAL: {
        rep_u32(rep, 0, MSG_SERIAL_STATUS);
        rep_u32(rep, 4, 0u);   /* client_slot 0: status of default port */
        /* E5-S8: ppcall stubbed */
        out[0].dev_type   = CC_DEV_TYPE_SERIAL;
        out[0].dev_handle = 0u;
        out[0].state      = (uint32_t)msg_u32(req, 0);
        out[0]._reserved  = 0u;
        rep_u32(rep, 0, 1u);
        break;
    }
    case CC_DEV_TYPE_NET: {
        rep_u32(rep, 0, MSG_NET_DEV_STATUS);
        rep_u32(rep, 4, 0u);   /* handle 0 */
        /* E5-S8: ppcall stubbed */
        out[0].dev_type   = CC_DEV_TYPE_NET;
        out[0].dev_handle = 0u;
        out[0].state      = (uint32_t)msg_u32(req, 0);
        out[0]._reserved  = 0u;
        rep_u32(rep, 0, 1u);
        break;
    }
    case CC_DEV_TYPE_BLOCK: {
        rep_u32(rep, 0, MSG_BLOCK_STATUS);
        rep_u32(rep, 4, 0u);   /* handle 0 */
        /* E5-S8: ppcall stubbed */
        out[0].dev_type   = CC_DEV_TYPE_BLOCK;
        out[0].dev_handle = 0u;
        out[0].state      = (uint32_t)msg_u32(req, 0);
        out[0]._reserved  = 0u;
        rep_u32(rep, 0, 1u);
        break;
    }
    case CC_DEV_TYPE_USB: {
        rep_u32(rep, 0, MSG_USB_LIST);
        /* E5-S8: ppcall stubbed */
        uint32_t usb_count = (uint32_t)msg_u32(req, 0);
        /* USB entries from cc_dev_shmem → re-encode as cc_device_info_t */
        const uint32_t *usb = (const uint32_t *)cc_dev_shmem_vaddr;
        for (uint32_t i = 0; i < usb_count; i++) {
            out[i].dev_type   = CC_DEV_TYPE_USB;
            out[i].dev_handle = usb[i * 4];
            out[i].state      = usb[i * 4 + 1];
            out[i]._reserved  = 0u;
        }
        rep_u32(rep, 0, usb_count);
        break;
    }
    case CC_DEV_TYPE_FB: {
        /* No bulk-list operation for framebuffers; report 0 devices */
        rep_u32(rep, 0, 0u);
        break;
    }
    default:
        rep_u32(rep, 0, CC_ERR_BAD_DEV_TYPE);
        break;
    }
}

/*
 * MSG_CC_LIST_POLECATS → MSG_AGENTPOOL_STATUS
 * Relays total/busy/idle counts from agent_pool.
 */
static void handle_list_polecats(void)
{
    rep_u32(rep, 0, MSG_AGENTPOOL_STATUS);
    /* E5-S8: ppcall stubbed */
    /* MR0=total MR1=busy MR2=idle from agent_pool */
    uint32_t total = (uint32_t)msg_u32(req, 0);
    uint32_t busy  = (uint32_t)msg_u32(req, 4);
    uint32_t idle  = (uint32_t)msg_u32(req, 8);
    rep_u32(rep, 0, CC_OK);
    rep_u32(rep, 4, total);
    rep_u32(rep, 8, busy);
    rep_u32(rep, 12, idle);
}

/*
 * MSG_CC_GUEST_STATUS → MSG_VIBEOS_STATUS
 * MR1=guest_handle → relays to vibe_engine; copies vibeos_status from
 * cc_vibe_shmem to cc_shmem as cc_guest_status_t.
 */
static void handle_guest_status(void)
{
    uint32_t guest_handle = (uint32_t)msg_u32(req, 4);

    rep_u32(rep, 0, MSG_VIBEOS_STATUS);
    rep_u32(rep, 4, guest_handle);
    /* E5-S8: ppcall stubbed */
    uint32_t ok = (uint32_t)msg_u32(req, 0);

    /* MSG_VIBEOS_STATUS: MR0=state; vibeos_status_reply in cc_vibe_shmem.
     * vibeos_status_reply layout: ok, state, os_type, ram_mb, uptime_ticks */
    const struct vibeos_status_reply *vs =
        (const struct vibeos_status_reply *)cc_vibe_shmem_vaddr;

    if (vs->ok != (uint32_t)VIBEOS_OK) {
        rep_u32(rep, 0, CC_ERR_RELAY_FAULT);
        return;
    }

    cc_guest_status_t *dst = (cc_guest_status_t *)cc_shmem_vaddr;
    dst->guest_handle  = guest_handle;
    dst->state         = vs->state;
    dst->os_type       = vs->os_type;
    dst->arch          = 0u;   /* vibeos_status_reply has no arch field */
    dst->device_flags  = 0u;   /* not surfaced by MSG_VIBEOS_STATUS */
    dst->_reserved[0]  = 0u;
    dst->_reserved[1]  = 0u;
    dst->_reserved[2]  = 0u;

    rep_u32(rep, 0, CC_OK);
}

/*
 * MSG_CC_DEVICE_STATUS → device PD selected by dev_type.
 * MR1=dev_type MR2=dev_handle → routes to appropriate PD status opcode.
 * Raw PD response is placed in cc_shmem for the caller to interpret.
 */
static void handle_device_status(void)
{
    uint32_t dev_type   = (uint32_t)msg_u32(req, 4);
    uint32_t dev_handle = (uint32_t)msg_u32(req, 8);
    uint32_t *out       = (uint32_t *)cc_shmem_vaddr;

    switch (dev_type) {
    case CC_DEV_TYPE_SERIAL:
        rep_u32(rep, 0, MSG_SERIAL_STATUS);
        rep_u32(rep, 4, dev_handle);
        /* E5-S8: ppcall stubbed */
        out[0] = (uint32_t)msg_u32(req, 0);
        out[1] = (uint32_t)msg_u32(req, 4);
        out[2] = (uint32_t)msg_u32(req, 8);
        rep_u32(rep, 0, CC_OK);
        break;

    case CC_DEV_TYPE_NET:
        rep_u32(rep, 0, MSG_NET_DEV_STATUS);
        rep_u32(rep, 4, dev_handle);
        /* E5-S8: ppcall stubbed */
        out[0] = (uint32_t)msg_u32(req, 0);
        out[1] = (uint32_t)msg_u32(req, 4);
        out[2] = (uint32_t)msg_u32(req, 8);
        rep_u32(rep, 0, CC_OK);
        break;

    case CC_DEV_TYPE_BLOCK:
        rep_u32(rep, 0, MSG_BLOCK_STATUS);
        rep_u32(rep, 4, dev_handle);
        /* E5-S8: ppcall stubbed */
        out[0] = (uint32_t)msg_u32(req, 0);
        out[1] = (uint32_t)msg_u32(req, 4);
        out[2] = (uint32_t)msg_u32(req, 8);
        rep_u32(rep, 0, CC_OK);
        break;

    case CC_DEV_TYPE_USB:
        rep_u32(rep, 0, MSG_USB_STATUS);
        rep_u32(rep, 4, dev_handle);
        /* E5-S8: ppcall stubbed */
        out[0] = (uint32_t)msg_u32(req, 0);
        out[1] = (uint32_t)msg_u32(req, 4);
        out[2] = (uint32_t)msg_u32(req, 8);
        rep_u32(rep, 0, CC_OK);
        break;

    case CC_DEV_TYPE_FB:
        /* framebuffer status: use MSG_FB_FLIP probe — just verify handle valid */
        rep_u32(rep, 0, CC_OK);
        out[0] = dev_handle;
        break;

    default:
        rep_u32(rep, 0, CC_ERR_BAD_DEV_TYPE);
        break;
    }
}

/*
 * MSG_CC_ATTACH_FRAMEBUFFER → MSG_FB_FLIP (handle probe) on framebuffer_pd.
 * MR1=guest_handle MR2=fb_handle
 *
 * Validates that fb_handle is a live framebuffer by issuing a non-mutating
 * probe; then records the subscription in cc_shmem for the caller.
 * Actual EVENT_FB_FRAME_READY delivery is via EventBus (out-of-band).
 */
static void handle_attach_framebuffer(void)
{
    uint32_t guest_handle = (uint32_t)msg_u32(req, 4);
    uint32_t fb_handle    = (uint32_t)msg_u32(req, 8);

    (void)guest_handle;   /* relay only: guest association is tracked by caller */

    /* Probe the framebuffer handle validity via MSG_FB_FLIP */
    rep_u32(rep, 0, MSG_FB_FLIP);
    rep_u32(rep, 4, fb_handle);
    /* E5-S8: ppcall stubbed */

    uint32_t fb_ok  = (uint32_t)msg_u32(req, 0);
    uint32_t fr_seq = (uint32_t)msg_u32(req, 4);

    if (fb_ok != (uint32_t)FB_OK) {
        rep_u32(rep, 0, CC_ERR_RELAY_FAULT);
        rep_u32(rep, 4, 0u);
        return;
    }

    rep_u32(rep, 0, CC_OK);
    rep_u32(rep, 4, fr_seq);
}

/*
 * MSG_CC_SEND_INPUT → MSG_GUEST_SEND_INPUT on guest_pd.
 * MR1=guest_handle; cc_input_event_t in cc_shmem → relayed via cc_guest_shmem.
 */
static void handle_send_input(void)
{
    uint32_t guest_handle = (uint32_t)msg_u32(req, 4);

    /* Copy input event from caller's cc_shmem to cc_guest_shmem */
    const cc_input_event_t *src = (const cc_input_event_t *)cc_shmem_vaddr;
    cc_input_event_t *dst       = (cc_input_event_t *)cc_guest_shmem_vaddr;
    *dst = *src;

    rep_u32(rep, 0, MSG_GUEST_SEND_INPUT);
    rep_u32(rep, 4, guest_handle);
    /* E5-S8: ppcall stubbed */

    uint32_t ok = (uint32_t)msg_u32(req, 0);
    rep_u32(rep, 0, ok == 0u ? CC_OK : CC_ERR_RELAY_FAULT);
}

/*
 * MSG_CC_SNAPSHOT → MSG_VIBEOS_SNAPSHOT on vibe_engine.
 * MR1=guest_handle → MR0=ok MR1=snap_lo MR2=snap_hi
 */
static void handle_snapshot(void)
{
    uint32_t guest_handle = (uint32_t)msg_u32(req, 4);

    rep_u32(rep, 0, MSG_VIBEOS_SNAPSHOT);
    rep_u32(rep, 4, guest_handle);
    /* E5-S8: ppcall stubbed */

    uint32_t ok      = (uint32_t)msg_u32(req, 0);
    uint32_t snap_lo = (uint32_t)msg_u32(req, 4);
    uint32_t snap_hi = (uint32_t)msg_u32(req, 8);

    rep_u32(rep, 0, ok == 0u ? CC_OK : CC_ERR_RELAY_FAULT);
    rep_u32(rep, 4, snap_lo);
    rep_u32(rep, 8, snap_hi);
}

/*
 * MSG_CC_RESTORE → MSG_VIBEOS_RESTORE on vibe_engine.
 * MR1=guest_handle MR2=snap_lo MR3=snap_hi → MR0=ok
 */
static void handle_restore(void)
{
    uint32_t guest_handle = (uint32_t)msg_u32(req, 4);
    uint32_t snap_lo      = (uint32_t)msg_u32(req, 8);
    uint32_t snap_hi      = (uint32_t)msg_u32(req, 12);

    rep_u32(rep, 0, MSG_VIBEOS_RESTORE);
    rep_u32(rep, 4, guest_handle);
    rep_u32(rep, 8, snap_lo);
    rep_u32(rep, 12, snap_hi);
    /* E5-S8: ppcall stubbed */

    uint32_t ok = (uint32_t)msg_u32(req, 0);
    rep_u32(rep, 0, ok == 0u ? CC_OK : CC_ERR_RELAY_FAULT);
}

/*
 * MSG_CC_LOG_STREAM → OP_LOG_WRITE on log_drain.
 * MR1=slot MR2=pd_id → MR0=ok MR1=bytes_drained
 */
static void handle_log_stream(void)
{
    uint32_t slot  = (uint32_t)msg_u32(req, 4);
    uint32_t pd_id = (uint32_t)msg_u32(req, 8);

    rep_u32(rep, 0, (uint32_t)OP_LOG_WRITE);
    rep_u32(rep, 4, slot);
    rep_u32(rep, 8, pd_id);
    /* E5-S8: ppcall stubbed */

    uint32_t ok    = (uint32_t)msg_u32(req, 0);
    uint32_t bytes = (uint32_t)msg_u32(req, 4);

    rep_u32(rep, 0, ok == 0u ? CC_OK : CC_ERR_RELAY_FAULT);
    rep_u32(rep, 4, bytes);
}

/* ─── Microkit entry points ──────────────────────────────────────────────── */

static void cc_pd_pd_init(void)
{
    sel4_dbg_puts("[cc_pd] starting — agentOS command-and-control relay\n");

    for (uint32_t i = 0; i < CC_MAX_SESSIONS; i++) {
        sessions[i].active = false;
        sessions[i].state  = CC_SESSION_STATE_IDLE;
    }

    sel4_dbg_puts("[cc_pd] ready (pure relay; zero policy)\n");
}

static void cc_pd_pd_notified(uint32_t ch)
{
    (void)ch;
}

static uint32_t cc_pd_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx)
{
    (void)ctx;

    uint32_t op = (uint32_t)msg_u32(req, 0);
    uint32_t ch = (uint32_t)b;  /* badge encodes caller channel/identity */

    switch (op) {
    /* Session management */
    case MSG_CC_CONNECT:    handle_connect(ch);        break;
    case MSG_CC_DISCONNECT: handle_disconnect(ch);     break;
    case MSG_CC_STATUS:     handle_status(ch);         break;
    case MSG_CC_LIST:       handle_list_sessions();    break;

    /* Direct relay API */
    case MSG_CC_LIST_GUESTS:        handle_list_guests();         break;
    case MSG_CC_LIST_DEVICES:       handle_list_devices();        break;
    case MSG_CC_LIST_POLECATS:      handle_list_polecats();       break;
    case MSG_CC_GUEST_STATUS:       handle_guest_status();        break;
    case MSG_CC_DEVICE_STATUS:      handle_device_status();       break;
    case MSG_CC_ATTACH_FRAMEBUFFER: handle_attach_framebuffer();  break;
    case MSG_CC_SEND_INPUT:         handle_send_input();          break;
    case MSG_CC_SNAPSHOT:           handle_snapshot();            break;
    case MSG_CC_RESTORE:            handle_restore();             break;
    case MSG_CC_LOG_STREAM:         handle_log_stream();          break;

    default:
        sel4_dbg_puts("[cc_pd] unknown opcode\n");
        rep_u32(rep, 0, CC_ERR_BAD_SESSION);
        break;
    }

    rep->length = 16;
        return SEL4_ERR_OK;
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void cc_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    cc_pd_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, cc_pd_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
