/*
 * agentOS Controller Protection Domain — raw seL4 IPC (E5-S2)
 *
 * Priority 50. Orchestrates the boot sequence and calls every other service.
 * All Microkit ppcall/notify calls have been replaced with sel4_client_call()
 * and seL4_Signal() using endpoints looked up from the nameserver.
 *
 * Entry point: controller_main(my_ep, ns_ep)
 *
 * Inbound requests are dispatched through a sel4_server_t dispatch loop.
 * Outbound calls use a sel4_client_t that caches nameserver lookups.
 */

#define AGENTOS_DEBUG 1

#ifndef AGENTOS_TEST_HOST
#include "sel4_ipc.h"
#include "sel4_server.h"
#include "sel4_client.h"
#include "nameserver.h"
#include "cap_policy.h"
#include "contracts/vmm_contract.h"
#include "boot_integrity.h"
#include "app_manager.h"
#include "verify.h"
#include "monocypher.h"
#else
/* ── Host-test stubs ─────────────────────────────────────────────────────── */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* Minimal sel4_ipc types for host builds */
#define SEL4_ERR_OK         0u
#define SEL4_ERR_INVALID_OP 1u
#define SEL4_ERR_NOT_FOUND  2u
#define SEL4_ERR_PERM       3u
#define SEL4_ERR_BAD_ARG    4u
#define SEL4_ERR_NO_MEM     5u
#define SEL4_ERR_BUSY       6u
#define SEL4_ERR_INTERNAL   8u

#define SEL4_MSG_DATA_BYTES 48u

typedef uint64_t seL4_CPtr;
typedef uint64_t seL4_Word;
typedef uint64_t sel4_badge_t;

typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[SEL4_MSG_DATA_BYTES];
} sel4_msg_t;

/* Stub nameserver constants */
#define NS_OK            0u
#define NS_ERR_NOT_FOUND 2u
#define NS_SVC_EVENTBUS  "event_bus"
#define NS_SVC_VFS       "vfs"
#define NS_SVC_NET       "net"
#define NS_SVC_SPAWN     "spawn"
#define NS_SVC_APPMANAGER "app_manager"
#define NS_SVC_HTTP      "http"
#define NS_SVC_AGENTFS   "agentfs"
#define NS_SVC_VIBEENGINE "vibe_engine"
#define CAP_CLASS_FS     (1 << 0)
#define CAP_CLASS_NET    (1 << 1)
#define CAP_CLASS_GPU    (1 << 2)
#define CAP_CLASS_IPC    (1 << 3)
#define CAP_CLASS_SPAWN  (1 << 6)
#define NS_VERSION       1u

/* Stub cap_policy */
#define CAP_POLICY_MAGIC   0xCAB01CA5u
#define CAP_POLICY_VERSION 1u
#define CAP_POLICY_MAX_GRANTS 64u
typedef struct { uint32_t magic; uint32_t version; uint32_t num_grants; } cap_policy_header_t;
typedef struct { uint32_t agent_id; uint8_t cap_class; uint8_t rights; } cap_grant_t;

/* Stub app_manager */
#define APP_OK 0u
#define OP_APP_LAUNCH 0xC0u

/* Stub verify */
static inline int verify_capabilities_manifest(const uint8_t *b, uint32_t l) { (void)b; (void)l; return -1; }

/* Stub monocypher */
static inline void crypto_ed25519_public_key(uint8_t *pk, const uint8_t *sk) { (void)pk; (void)sk; }
static inline void crypto_ed25519_sign(uint8_t *s, const uint8_t *sk, const uint8_t *pk,
                                        const uint8_t *m, size_t l) { (void)s;(void)sk;(void)pk;(void)m;(void)l; }
static inline int  crypto_ed25519_check(const uint8_t *s, const uint8_t *m, size_t l,
                                         const uint8_t *pk) { (void)s;(void)m;(void)l;(void)pk; return 0; }

/* Stub boot_integrity */
static inline void boot_integrity_init(void) {}

/* Stub cap_broker */
static inline void cap_broker_init(void) {}
static inline void cap_broker_revoke_agent(uint32_t a, uint32_t r) { (void)a;(void)r; }
static inline uint32_t cap_broker_attest(uint64_t t, uint32_t n, uint32_t d) { (void)t;(void)n;(void)d; return 0; }

/* Stub agent_pool */
static inline void agent_pool_init(void) {}
static inline int  agent_pool_spawn(const char *n, uint64_t t,
                                     const uint8_t *p, uint32_t l, uint32_t prio) {
    (void)n;(void)t;(void)p;(void)l;(void)prio; return 0;
}

/* Stub agentos_log_boot */
static inline void agentos_log_boot(const char *n) { (void)n; }

/* seL4_DebugPutChar stub */
static inline void seL4_DebugPutChar(char c) { (void)c; }

/* sel4_client / sel4_server minimal stubs for host tests */
#define SEL4_CLIENT_CACHE_SIZE 16u
#define SEL4_CLIENT_NAME_MAX   48u

typedef struct { char name[SEL4_CLIENT_NAME_MAX]; seL4_CPtr ep; uint32_t valid; } sel4_client_entry_t;
typedef struct {
    sel4_client_entry_t entries[SEL4_CLIENT_CACHE_SIZE];
    seL4_CPtr nameserver_ep;
    seL4_CPtr my_cnode;
    seL4_Word next_free_slot;
} sel4_client_t;

static inline void sel4_client_init(sel4_client_t *c, seL4_CPtr ns, seL4_CPtr cn, seL4_Word slot) {
    (void)ns;(void)cn;(void)slot;
    for (uint32_t i = 0; i < SEL4_CLIENT_CACHE_SIZE; i++) { c->entries[i].valid = 0; c->entries[i].ep = 0; c->entries[i].name[0] = '\0'; }
    c->nameserver_ep = ns; c->my_cnode = cn; c->next_free_slot = slot;
}
/* Returns SEL4_ERR_OK and ep=0 for host tests (no real nameserver) */
static inline uint32_t sel4_client_connect(sel4_client_t *c, const char *name, seL4_CPtr *ep) {
    (void)c;(void)name; *ep = 0; return SEL4_ERR_OK;
}
static inline uint32_t sel4_client_call(seL4_CPtr ep, uint32_t op,
                                         const void *payload, uint32_t len,
                                         sel4_msg_t *rep) {
    (void)ep;(void)op;(void)payload;(void)len;
    rep->opcode = SEL4_ERR_OK; rep->length = 0;
    return SEL4_ERR_OK;
}

/* sel4_server stubs */
#define SEL4_SERVER_MAX_HANDLERS 32u
typedef uint32_t (*sel4_handler_fn)(sel4_badge_t, const sel4_msg_t *, sel4_msg_t *, void *);
typedef struct {
    struct { uint32_t opcode; sel4_handler_fn fn; void *ctx; } handlers[SEL4_SERVER_MAX_HANDLERS];
    uint32_t handler_count;
    seL4_CPtr ep;
} sel4_server_t;
static inline void sel4_server_init(sel4_server_t *s, seL4_CPtr ep) {
    s->handler_count = 0; s->ep = ep;
    for (uint32_t i = 0; i < SEL4_SERVER_MAX_HANDLERS; i++) {
        s->handlers[i].opcode = 0; s->handlers[i].fn = (sel4_handler_fn)0; s->handlers[i].ctx = (void *)0;
    }
}
static inline int sel4_server_register(sel4_server_t *s, uint32_t op, sel4_handler_fn fn, void *ctx) {
    if (s->handler_count >= SEL4_SERVER_MAX_HANDLERS) return -1;
    s->handlers[s->handler_count].opcode = op;
    s->handlers[s->handler_count].fn = fn;
    s->handlers[s->handler_count].ctx = ctx;
    s->handler_count++; return 0;
}
static inline uint32_t sel4_server_dispatch(sel4_server_t *s, sel4_badge_t badge,
                                              const sel4_msg_t *req, sel4_msg_t *rep) {
    for (uint32_t i = 0; i < s->handler_count; i++) {
        if (s->handlers[i].opcode == req->opcode) {
            uint32_t rc = s->handlers[i].fn(badge, req, rep, s->handlers[i].ctx);
            rep->opcode = rc; return rc;
        }
    }
    rep->opcode = SEL4_ERR_INVALID_OP; rep->length = 0; return SEL4_ERR_INVALID_OP;
}
/* sel4_server_run is intentionally omitted for host tests (would loop forever) */

/* seL4_Signal stub */
static inline void seL4_Signal(seL4_CPtr cap) { (void)cap; }

/* MSG_* and OP_* constants needed by monitor.c logic */
#define MSG_EVENTBUS_INIT          0x0001u
#define MSG_EVENTBUS_READY         0x0101u
#define MSG_INITAGENT_START        0x0201u
#define MSG_EVENT_AGENT_EXITED     0x0402u
#define MSG_SPAWN_AGENT            0x0801u
#define MSG_SPAWN_AGENT_REPLY      0x0802u
#define MSG_WORKER_RETRIEVE        0x0701u
#define MSG_WORKER_RETRIEVE_REPLY  0x0702u
#define MSG_QUOTA_REVOKE           0x0B01u
#define MSG_GPU_SUBMIT             0x0901u
#define MSG_VMM_VCPU_SET_REGS      0x2B05u
#define MSG_VMM_REGISTER           0x2B01u
#define EVT_OBJECT_CREATED         0x0411u
#define OP_AGENTFS_PUT             0x30u
#define OP_AGENTFS_GET             0x31u
#define OP_CAP_POLICY_RELOAD       0xC0u
#define TRACE_PD_CONTROLLER        0u
#define TRACE_PD_EVENT_BUS         1u
#define TRACE_PD_INIT_AGENT        2u
#define TRACE_PD_WORKER_0          3u
#define TRACE_PD_VFS_SERVER        27u
#define TRACE_PD_VIRTIO_BLK        28u
#define TRACE_PD_SPAWN_SERVER      29u
#define TRACE_PD_NET_SERVER        30u
#define TRACE_PD_APP_MANAGER       31u
#define TRACE_PD_HTTP_SVC          32u
#define TRACE_PD_AGENTFS           11u
#define TRACE_PD_SWAP_SLOT_0       13u

/* Stub vcpu_regs_t for VMM contract */
#ifndef VMM_CONTRACT_H
typedef struct { uint64_t spsr; } vcpu_regs_t;
static inline int cap_policy_vcpu_el_check(uint64_t spsr, bool aarch64) { (void)spsr;(void)aarch64; return 0; }
#define AGENTOS_CRIT(s) do {} while(0)
#endif

/* NUM_SWAP_SLOTS */
#define NUM_SWAP_SLOTS 4

#endif /* AGENTOS_TEST_HOST */

/* ─────────────────────────────────────────────────────────────────────────────
 * Memory regions (set by root-task on real hardware; zeroed in host tests)
 * ─────────────────────────────────────────────────────────────────────────── */
#ifndef AGENTOS_TEST_HOST
uintptr_t monitor_stack_vaddr;
uintptr_t swap_code_ctrl_0;
uintptr_t swap_code_ctrl_1;
uintptr_t swap_code_ctrl_2;
uintptr_t swap_code_ctrl_3;
uintptr_t vibe_staging_ctrl_vaddr;
uintptr_t cap_policy_shmem_vaddr;
uintptr_t ns_registry_shmem_ctrl_vaddr;
uintptr_t vfs_io_shmem_ctrl_vaddr;
uintptr_t spawn_elf_shmem_ctrl_vaddr;
uintptr_t spawn_config_shmem_ctrl_vaddr;
uintptr_t net_packet_shmem_ctrl_vaddr;
uintptr_t http_req_shmem_ctrl_vaddr;
uintptr_t app_manifest_shmem_ctrl_vaddr;
uintptr_t ext2_shmem_ctrl_vaddr;
uintptr_t vm_list_shmem_ctrl_vaddr;
uintptr_t vmm_vcpu_regs_vaddr;
uintptr_t block_shmem_ctrl_vaddr;
uintptr_t log_drain_rings_vaddr;
#else
/* Host test: expose as simple globals so test code can set them */
uintptr_t monitor_stack_vaddr;
uintptr_t vibe_staging_ctrl_vaddr;
uintptr_t cap_policy_shmem_vaddr;
uintptr_t app_manifest_shmem_ctrl_vaddr;
uintptr_t vmm_vcpu_regs_vaddr;
uintptr_t log_drain_rings_vaddr;
uintptr_t swap_code_ctrl_0;
uintptr_t swap_code_ctrl_1;
uintptr_t swap_code_ctrl_2;
uintptr_t swap_code_ctrl_3;
#endif

/* ─────────────────────────────────────────────────────────────────────────────
 * Echo service WASM binary (embedded; 305 bytes)
 * ─────────────────────────────────────────────────────────────────────────── */
static const uint8_t ECHO_SERVICE_WASM[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x21, 0x06, 0x60,
    0x03, 0x7f, 0x7f, 0x7f, 0x00, 0x60, 0x00, 0x01, 0x7e, 0x60, 0x03, 0x7f,
    0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x00, 0x60, 0x05, 0x7e, 0x7e, 0x7e,
    0x7e, 0x7e, 0x00, 0x60, 0x00, 0x01, 0x7f, 0x02, 0x48, 0x04, 0x03, 0x61,
    0x6f, 0x73, 0x07, 0x61, 0x6f, 0x73, 0x5f, 0x6c, 0x6f, 0x67, 0x00, 0x00,
    0x03, 0x61, 0x6f, 0x73, 0x0b, 0x61, 0x6f, 0x73, 0x5f, 0x74, 0x69, 0x6d,
    0x65, 0x5f, 0x75, 0x73, 0x00, 0x01, 0x03, 0x61, 0x6f, 0x73, 0x0c, 0x61,
    0x6f, 0x73, 0x5f, 0x6d, 0x65, 0x6d, 0x5f, 0x72, 0x65, 0x61, 0x64, 0x00,
    0x02, 0x03, 0x61, 0x6f, 0x73, 0x0d, 0x61, 0x6f, 0x73, 0x5f, 0x6d, 0x65,
    0x6d, 0x5f, 0x77, 0x72, 0x69, 0x74, 0x65, 0x00, 0x02, 0x03, 0x04, 0x03,
    0x03, 0x04, 0x05, 0x05, 0x03, 0x01, 0x00, 0x21, 0x07, 0x2d, 0x04, 0x06,
    0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x04, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x04, 0x0a, 0x68, 0x61, 0x6e, 0x64, 0x6c, 0x65, 0x5f, 0x70,
    0x70, 0x63, 0x00, 0x05, 0x0c, 0x68, 0x65, 0x61, 0x6c, 0x74, 0x68, 0x5f,
    0x63, 0x68, 0x65, 0x63, 0x6b, 0x00, 0x06, 0x0a, 0x49, 0x03, 0x0a, 0x00,
    0x41, 0x00, 0x41, 0x00, 0x41, 0x2f, 0x10, 0x00, 0x0b, 0x37, 0x00, 0x41,
    0x80, 0x80, 0x80, 0x01, 0x20, 0x00, 0x37, 0x03, 0x00, 0x41, 0x88, 0x80,
    0x80, 0x01, 0x20, 0x01, 0x42, 0x01, 0x7c, 0x37, 0x03, 0x00, 0x41, 0x90,
    0x80, 0x80, 0x01, 0x20, 0x02, 0x37, 0x03, 0x00, 0x41, 0x98, 0x80, 0x80,
    0x01, 0x20, 0x03, 0x37, 0x03, 0x00, 0x41, 0xa0, 0x80, 0x80, 0x01, 0x20,
    0x04, 0x37, 0x03, 0x00, 0x0b, 0x04, 0x00, 0x41, 0x01, 0x0b, 0x0b, 0x35,
    0x01, 0x00, 0x41, 0x00, 0x0b, 0x2f, 0x45, 0x63, 0x68, 0x6f, 0x20, 0x73,
    0x65, 0x72, 0x76, 0x69, 0x63, 0x65, 0x20, 0x69, 0x6e, 0x69, 0x74, 0x69,
    0x61, 0x6c, 0x69, 0x7a, 0x65, 0x64, 0x20, 0x76, 0x69, 0x61, 0x20, 0x61,
    0x67, 0x65, 0x6e, 0x74, 0x4f, 0x53, 0x20, 0x76, 0x69, 0x62, 0x65, 0x2d,
    0x73, 0x77, 0x61, 0x70, 0x21
};
static const uint32_t ECHO_SERVICE_WASM_LEN = 305u;

/* ─────────────────────────────────────────────────────────────────────────────
 * Global seL4 IPC objects
 * ─────────────────────────────────────────────────────────────────────────── */

/* Client: outbound service calls */
static sel4_client_t g_client;

/* Server: inbound dispatch loop */
static sel4_server_t g_srv;

/*
 * Cached endpoint capabilities for services the controller calls.
 * Populated during controller_main startup via sel4_client_connect().
 */
static seL4_CPtr g_ep_eventbus;   /* event_bus PD endpoint    */
static seL4_CPtr g_ep_agentfs;    /* agentfs PD endpoint      */
static seL4_CPtr g_ep_vfs;        /* vfs_server endpoint      */
static seL4_CPtr g_ep_net;        /* net_server endpoint      */
static seL4_CPtr g_ep_spawn;      /* spawn_server endpoint    */
static seL4_CPtr g_ep_appmgr;     /* app_manager endpoint     */
static seL4_CPtr g_ep_http;       /* http_svc endpoint        */

/*
 * Notification capability for event_bus (used for seL4_Signal instead of
 * microkit_notify).  Obtained from nameserver at startup.
 * On real hardware this is a badged notification cap minted by the nameserver.
 * In host tests it stays 0 and seL4_Signal is a no-op.
 */
static seL4_CPtr g_event_bus_ntfn_cap;
static seL4_CPtr g_initagent_ntfn_cap;
static seL4_CPtr g_net_timer_ntfn_cap;

/* ─────────────────────────────────────────────────────────────────────────────
 * Controller runtime state
 * ─────────────────────────────────────────────────────────────────────────── */

static struct {
    bool     eventbus_ready;
    bool     initagent_ready;
    uint32_t notification_count;
    /* Demo object ID from AgentFS */
    uint32_t demo_obj_id[4];
    bool     demo_obj_stored;
    /* Worker / demo state */
    bool     worker_task_dispatched;
    bool     demo_complete;
    /* VibeEngine demo state */
    bool     vibe_demo_triggered;
    bool     vibe_swap_in_progress;
    bool     vibe_demo_complete;
    /* lwIP tick counter */
    uint32_t net_tick_counter;
} ctrl;

/* Periodic lwIP timer tick period */
#define NET_TICK_INTERVAL 10u

/* ─────────────────────────────────────────────────────────────────────────────
 * Debug output helpers (no Microkit API)
 * ─────────────────────────────────────────────────────────────────────────── */

static void ctrl_puts(const char *s)
{
#ifndef AGENTOS_TEST_HOST
    for (const char *p = s; *p; p++)
        seL4_DebugPutChar(*p);
#else
    (void)s;
#endif
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Forward declarations (vibe-swap and cap-broker subsystems)
 * These are implemented elsewhere in the root-task source tree.
 * ─────────────────────────────────────────────────────────────────────────── */
void vibe_swap_init(void);
int  vibe_swap_begin(uint32_t service_id, const void *code, uint32_t code_len);
int  vibe_swap_health_notify(int slot);
int  vibe_swap_rollback(uint32_t service_id);

/* ─────────────────────────────────────────────────────────────────────────────
 * Utility: demo_delay
 * ─────────────────────────────────────────────────────────────────────────── */

static void demo_delay(void) {
    for (volatile uint32_t i = 0; i < 100000; i++) {
        __asm__ volatile("" ::: "memory");
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Utility: put_hex_byte, put_uint32_dec
 * ─────────────────────────────────────────────────────────────────────────── */

static void put_hex_byte(uint8_t b)
{
    static const char hex[] = "0123456789abcdef";
    char buf[3];
    buf[0] = hex[(b >> 4) & 0xf];
    buf[1] = hex[b & 0xf];
    buf[2] = '\0';
    ctrl_puts(buf);
}

/* Print uint32 as decimal into caller-supplied buffer (must be >=12 bytes). */
static void uint32_to_dec(uint32_t v, char *out, int out_sz)
{
    char tmp[12]; int ti = 0;
    if (v == 0) { tmp[ti++] = '0'; }
    else { while (v > 0 && ti < 11) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; } }
    int o = 0;
    for (int i = ti - 1; i >= 0 && o + 1 < out_sz; i--)
        out[o++] = tmp[i];
    out[o] = '\0';
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Policy helpers
 * ─────────────────────────────────────────────────────────────────────────── */

static bool g_policy_loaded = false;

static void monitor_apply_default_policy(void)
{
    ctrl_puts("[monitor] applying hardcoded default policy\n");
    g_policy_loaded = true;
}

static void monitor_apply_policy(void)
{
#ifndef AGENTOS_TEST_HOST
    volatile cap_policy_header_t *hdr =
        (volatile cap_policy_header_t *)cap_policy_shmem_vaddr;

    if (!cap_policy_shmem_vaddr || hdr->magic != CAP_POLICY_MAGIC) {
        ctrl_puts("[monitor] no policy blob, using defaults\n");
        monitor_apply_default_policy();
        return;
    }
    if (hdr->version != CAP_POLICY_VERSION || hdr->num_grants > CAP_POLICY_MAX_GRANTS) {
        ctrl_puts("[monitor] invalid policy header, using defaults\n");
        monitor_apply_default_policy();
        return;
    }

    volatile cap_grant_t *grants = (volatile cap_grant_t *)(hdr + 1);
    for (uint32_t i = 0; i < hdr->num_grants; i++) {
        ctrl_puts("[monitor] policy grant agent=");
        char abuf[4];
        abuf[0] = (char)('0' + (grants[i].agent_id % 10));
        abuf[1] = '\0';
        ctrl_puts(abuf);
        ctrl_puts("\n");
    }
    g_policy_loaded = true;
    ctrl_puts("[monitor] applied policy grants\n");
#else
    monitor_apply_default_policy();
#endif
}

/* ─────────────────────────────────────────────────────────────────────────────
 * ns_register_service — register one service via nameserver client call
 * ─────────────────────────────────────────────────────────────────────────── */

static void ns_register_service(const char *svc_name,
                                 uint32_t    channel_id,
                                 uint32_t    pd_id,
                                 uint32_t    cap_classes)
{
    /*
     * Pack the register request into sel4_msg_t.data[]:
     *   data[0..3]   = channel_id (LE)
     *   data[4..7]   = pd_id (LE)
     *   data[8..11]  = cap_classes (LE)
     *   data[12..15] = version (LE) = 1
     *   data[16..47] = service name, NUL-padded
     */
    uint8_t payload[SEL4_MSG_DATA_BYTES];
    payload[0]  = (uint8_t)(channel_id & 0xff);
    payload[1]  = (uint8_t)((channel_id >> 8) & 0xff);
    payload[2]  = (uint8_t)((channel_id >> 16) & 0xff);
    payload[3]  = (uint8_t)((channel_id >> 24) & 0xff);
    payload[4]  = (uint8_t)(pd_id & 0xff);
    payload[5]  = (uint8_t)((pd_id >> 8) & 0xff);
    payload[6]  = (uint8_t)((pd_id >> 16) & 0xff);
    payload[7]  = (uint8_t)((pd_id >> 24) & 0xff);
    payload[8]  = (uint8_t)(cap_classes & 0xff);
    payload[9]  = (uint8_t)((cap_classes >> 8) & 0xff);
    payload[10] = (uint8_t)((cap_classes >> 16) & 0xff);
    payload[11] = (uint8_t)((cap_classes >> 24) & 0xff);
    payload[12] = 1u;  /* version low byte */
    payload[13] = 0u; payload[14] = 0u; payload[15] = 0u;

    /* Copy service name into data[16..47] */
    uint32_t ni = 16u;
    for (const char *p = svc_name; *p && ni < SEL4_MSG_DATA_BYTES - 1u; p++, ni++)
        payload[ni] = (uint8_t)*p;
    for (; ni < SEL4_MSG_DATA_BYTES; ni++)
        payload[ni] = 0u;

    sel4_msg_t rep;
#ifndef AGENTOS_TEST_HOST
    seL4_CPtr ns_ep;
    uint32_t conn_rc = sel4_client_connect(&g_client, "nameserver", &ns_ep);
    if (conn_rc != SEL4_ERR_OK) {
        ctrl_puts("[controller] ns_register: nameserver not found\n");
        return;
    }
    uint32_t rc = sel4_client_call(ns_ep, 0xD0u /* OP_NS_REGISTER */,
                                   payload, SEL4_MSG_DATA_BYTES, &rep);
#else
    uint32_t rc = sel4_client_call(0, 0xD0u, payload, SEL4_MSG_DATA_BYTES, &rep);
#endif
    if (rc != SEL4_ERR_OK && rc != NS_OK) {
        ctrl_puts("[controller] NS_REGISTER failed for: ");
        ctrl_puts(svc_name);
        ctrl_puts("\n");
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Demo helpers
 * ─────────────────────────────────────────────────────────────────────────── */

static void demo_sequence(void)
{
    ctrl_puts("\n"
              "======================================================\n"
              "  DEMO: Agent Data Flow — PDs exchanging real data\n"
              "======================================================\n\n");

    /* Step 1: Store object in AgentFS */
    ctrl_puts("[controller] Step 1: Storing object in AgentFS via IPC...\n");

    uint8_t put_payload[12];
    uint32_t obj_size = 18u;
    put_payload[0]  = (uint8_t)(OP_AGENTFS_PUT);
    put_payload[1]  = 0u; put_payload[2] = 0u; put_payload[3] = 0u;
    put_payload[4]  = (uint8_t)(obj_size & 0xff);
    put_payload[5]  = (uint8_t)((obj_size >> 8) & 0xff);
    put_payload[6]  = (uint8_t)((obj_size >> 16) & 0xff);
    put_payload[7]  = (uint8_t)((obj_size >> 24) & 0xff);
    put_payload[8]  = 0x42u; /* cap_tag */
    put_payload[9]  = 0u; put_payload[10] = 0u; put_payload[11] = 0u;

    sel4_msg_t rep;
    uint32_t rc = sel4_client_call(g_ep_agentfs, OP_AGENTFS_PUT,
                                   put_payload, sizeof(put_payload), &rep);
    if (rc == SEL4_ERR_OK) {
        /* Read back object ID from reply data */
        ctrl.demo_obj_id[0] = (uint32_t)rep.data[0]  | ((uint32_t)rep.data[1] << 8)
                            | ((uint32_t)rep.data[2] << 16) | ((uint32_t)rep.data[3] << 24);
        ctrl.demo_obj_id[1] = (uint32_t)rep.data[4]  | ((uint32_t)rep.data[5] << 8)
                            | ((uint32_t)rep.data[6] << 16) | ((uint32_t)rep.data[7] << 24);
        ctrl.demo_obj_id[2] = (uint32_t)rep.data[8]  | ((uint32_t)rep.data[9] << 8)
                            | ((uint32_t)rep.data[10] << 16)| ((uint32_t)rep.data[11] << 24);
        ctrl.demo_obj_id[3] = (uint32_t)rep.data[12] | ((uint32_t)rep.data[13] << 8)
                            | ((uint32_t)rep.data[14] << 16)| ((uint32_t)rep.data[15] << 24);
        ctrl.demo_obj_stored = true;
        ctrl_puts("[controller] AgentFS PUT OK — object id: 0x");
        put_hex_byte((ctrl.demo_obj_id[0] >> 24) & 0xff);
        put_hex_byte((ctrl.demo_obj_id[0] >> 16) & 0xff);
        put_hex_byte((ctrl.demo_obj_id[0] >>  8) & 0xff);
        put_hex_byte( ctrl.demo_obj_id[0]        & 0xff);
        ctrl_puts("...\n[controller] Object payload: 'Hello from agentOS' (18 bytes)\n");
    } else {
        ctrl_puts("[controller] AgentFS PUT FAILED\n");
        return;
    }

    demo_delay();

    /* Step 2: Publish event to EventBus via capability call */
    ctrl_puts("[controller] Step 2: Publishing OBJECT_CREATED event to EventBus...\n");
    {
        uint8_t evt_payload[12];
        evt_payload[0]  = (uint8_t)(EVT_OBJECT_CREATED & 0xff);
        evt_payload[1]  = (uint8_t)((EVT_OBJECT_CREATED >> 8) & 0xff);
        evt_payload[2]  = 0u; evt_payload[3] = 0u;
        evt_payload[4]  = (uint8_t)(ctrl.demo_obj_id[0] & 0xff);
        evt_payload[5]  = (uint8_t)((ctrl.demo_obj_id[0] >> 8) & 0xff);
        evt_payload[6]  = (uint8_t)((ctrl.demo_obj_id[0] >> 16) & 0xff);
        evt_payload[7]  = (uint8_t)((ctrl.demo_obj_id[0] >> 24) & 0xff);
        evt_payload[8]  = (uint8_t)(obj_size & 0xff);
        evt_payload[9]  = (uint8_t)((obj_size >> 8) & 0xff);
        evt_payload[10] = 0u; evt_payload[11] = 0u;
        sel4_client_call(g_ep_eventbus, (uint32_t)EVT_OBJECT_CREATED,
                         evt_payload, sizeof(evt_payload), &rep);
        ctrl_puts("[controller] Event published to ring buffer\n");
    }

    demo_delay();

    /* Step 3: Dispatch task to worker_0 via notification signal */
    ctrl_puts("[controller] Step 3: Dispatching task to worker_0 — 'retrieve object'\n");
    ctrl.worker_task_dispatched = true;
    /* Signal worker_0's notification cap (cap looked up at startup for pool slot 0) */
    seL4_Signal(g_initagent_ntfn_cap);  /* proxy: signal initagent which signals worker */
    ctrl_puts("[controller] Task dispatched. Waiting for worker completion...\n");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * microservice_demo — Step 5: AppManager launch
 * ─────────────────────────────────────────────────────────────────────────── */

static void microservice_demo(void)
{
    ctrl_puts("\n"
              "======================================================\n"
              "  DEMO Step 5: AppManager — full-stack app launch\n"
              "======================================================\n\n");

    if (!app_manifest_shmem_ctrl_vaddr) {
        ctrl_puts("[controller] app_manifest_shmem not mapped — skipping step 5\n");
        return;
    }

    static const char DEMO_MANIFEST[] =
        "name=echo-app\n"
        "elf=/apps/echo.elf\n"
        "http_prefix=/echo\n"
        "caps=35\n";
    static const uint32_t DEMO_MANIFEST_LEN = sizeof(DEMO_MANIFEST) - 1u;

#ifndef AGENTOS_TEST_HOST
    volatile uint8_t *dst = (volatile uint8_t *)app_manifest_shmem_ctrl_vaddr;
    for (uint32_t i = 0; i < DEMO_MANIFEST_LEN; i++)
        dst[i] = (uint8_t)DEMO_MANIFEST[i];
#endif

    ctrl_puts("[controller] Manifest written — calling OP_APP_LAUNCH...\n");

    uint8_t launch_payload[8];
    launch_payload[0] = (uint8_t)(DEMO_MANIFEST_LEN & 0xff);
    launch_payload[1] = (uint8_t)((DEMO_MANIFEST_LEN >> 8) & 0xff);
    launch_payload[2] = 0u; launch_payload[3] = 0u;
    launch_payload[4] = 0u; launch_payload[5] = 0u;
    launch_payload[6] = 0u; launch_payload[7] = 0u;

    sel4_msg_t rep;
    uint32_t rc = sel4_client_call(g_ep_appmgr, OP_APP_LAUNCH,
                                   launch_payload, sizeof(launch_payload), &rep);
    uint32_t app_id = (uint32_t)rep.data[0] | ((uint32_t)rep.data[1] << 8)
                    | ((uint32_t)rep.data[2] << 16) | ((uint32_t)rep.data[3] << 24);
    uint32_t vnic   = (uint32_t)rep.data[4] | ((uint32_t)rep.data[5] << 8)
                    | ((uint32_t)rep.data[6] << 16) | ((uint32_t)rep.data[7] << 24);

    if (rc == SEL4_ERR_OK) {
        ctrl_puts("[controller] APP_LAUNCH OK — app_id=");
        char buf[12]; uint32_to_dec(app_id, buf, sizeof(buf));
        ctrl_puts(buf);
        ctrl_puts(" vnic=");
        if (vnic == 0xFFFFFFFFu) { ctrl_puts("none"); }
        else { uint32_to_dec(vnic, buf, sizeof(buf)); ctrl_puts(buf); }
        ctrl_puts("\n[controller] echo-app registered at HTTP prefix /echo\n");
        ctrl_puts("\n"
                  "======================================================\n"
                  "  DEMO COMPLETE — All 5 steps passed!\n"
                  "  Step 1: AgentFS object store    — PUT/GET via IPC\n"
                  "  Step 2: EventBus pub/sub        — ring buffer + notify\n"
                  "  Step 3: Agent pool workers      — task dispatch + done\n"
                  "  Step 4: VibeEngine hot-swap     — WASM live via wasm3\n"
                  "  Step 5: AppManager launch       — echo-app at /echo\n"
                  "  PDs: 34 on seL4 (RISC-V / AArch64)\n"
                  "======================================================\n\n");
    } else {
        ctrl_puts("[controller] APP_LAUNCH failed (services may be stubs)\n");
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * vibe_demo_step4_notify — process VibeEngine swap notification
 * ─────────────────────────────────────────────────────────────────────────── */

static void vibe_demo_step4_notify(void)
{
    ctrl_puts("[controller] Step 4: VibeEngine approved a swap!\n");
    ctrl_puts("[controller] Reading proposal from staging region...\n");

#ifndef AGENTOS_TEST_HOST
    static const uint32_t STAGING_SIZE = 0x400000u;
    const volatile uint8_t *meta =
        (const volatile uint8_t *)(vibe_staging_ctrl_vaddr + STAGING_SIZE - 64u);

    uint32_t service_id  = (uint32_t)meta[0] | ((uint32_t)meta[1] << 8)
                         | ((uint32_t)meta[2] << 16) | ((uint32_t)meta[3] << 24);
    uint32_t wasm_offset = (uint32_t)meta[4] | ((uint32_t)meta[5] << 8)
                         | ((uint32_t)meta[6] << 16) | ((uint32_t)meta[7] << 24);
    uint32_t wasm_size   = (uint32_t)meta[8] | ((uint32_t)meta[9] << 8)
                         | ((uint32_t)meta[10] << 16) | ((uint32_t)meta[11] << 24);

    if (wasm_size == 0xFFFFFFFFu) {
        ctrl_puts("[controller] Rollback requested\n");
        vibe_swap_rollback(service_id);
        return;
    }

    const uint8_t *wasm_bytes =
        (const uint8_t *)(vibe_staging_ctrl_vaddr + wasm_offset);

    int manifest_ok = verify_capabilities_manifest(wasm_bytes, wasm_size);
    if (manifest_ok == -2) {
        ctrl_puts("[monitor] WASM manifest hash mismatch — rejecting agent load\n");
        return;
    } else if (manifest_ok == -1) {
        ctrl_puts("[monitor] no capability manifest — granting minimal defaults only\n");
    }

    ctrl_puts("[controller] Initiating kernel-side swap...\n");
    ctrl.vibe_swap_in_progress = true;
    int slot = vibe_swap_begin(service_id, wasm_bytes, wasm_size);
    if (slot < 0) {
        ctrl_puts("[controller] vibe_swap_begin FAILED\n");
        ctrl.vibe_swap_in_progress = false;
    } else {
        ctrl_puts("[controller] vibe_swap_begin OK\n");
    }
#else
    ctrl.vibe_swap_in_progress = true;
    ctrl_puts("[controller] (host test) vibe step4 notify processed\n");
#endif
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Inbound handler: OP_CAP_POLICY_RELOAD
 * ─────────────────────────────────────────────────────────────────────────── */

static uint32_t handle_cap_policy_reload(sel4_badge_t badge,
                                          const sel4_msg_t *req,
                                          sel4_msg_t *rep,
                                          void *ctx)
{
    (void)badge; (void)req; (void)ctx;
    monitor_apply_policy();
    rep->data[0] = g_policy_loaded ? 1u : 0u;
    rep->length  = 1u;
    return SEL4_ERR_OK;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Inbound handler: MSG_VMM_VCPU_SET_REGS
 * ─────────────────────────────────────────────────────────────────────────── */

static uint32_t handle_vmm_vcpu_set_regs(sel4_badge_t badge,
                                           const sel4_msg_t *req,
                                           sel4_msg_t *rep,
                                           void *ctx)
{
    (void)badge; (void)req; (void)ctx;
#ifndef AGENTOS_TEST_HOST
    if (!vmm_vcpu_regs_vaddr) {
        AGENTOS_CRIT("[cap_policy] vCPU regs shmem not mapped — EPERM");
        rep->data[0] = 0u;
        rep->length  = 1u;
        return SEL4_ERR_PERM;
    }
    {
        volatile vcpu_regs_t *regs = (volatile vcpu_regs_t *)vmm_vcpu_regs_vaddr;
#ifdef ARCH_AARCH64
        int el_rc = cap_policy_vcpu_el_check(regs->spsr, true);
#else
        int el_rc = cap_policy_vcpu_el_check(regs->spsr, false);
#endif
        if (el_rc != 0) {
            AGENTOS_CRIT("[cap_policy] vCPU EL2/CPL0 escalation REJECTED");
            rep->data[0] = 0u;
            rep->length  = 1u;
            return SEL4_ERR_PERM;
        }
    }
#endif
    rep->data[0] = 1u;
    rep->length  = 1u;
    return SEL4_ERR_OK;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Inbound handler: MSG_VMM_REGISTER
 * ─────────────────────────────────────────────────────────────────────────── */

static uint32_t handle_vmm_register(sel4_badge_t badge,
                                     const sel4_msg_t *req,
                                     sel4_msg_t *rep,
                                     void *ctx)
{
    (void)badge; (void)req; (void)ctx;
    rep->data[0] = 1u;  /* ok */
    rep->data[1] = 1u;  /* vmm_token */
    rep->data[2] = 1u;  /* granted_guests */
    rep->length  = 3u;
    return SEL4_ERR_OK;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Inbound handler: MSG_WORKER_RETRIEVE (proxy AgentFS GET for worker)
 * ─────────────────────────────────────────────────────────────────────────── */

static uint32_t handle_worker_retrieve(sel4_badge_t badge,
                                        const sel4_msg_t *req,
                                        sel4_msg_t *rep,
                                        void *ctx)
{
    (void)badge; (void)ctx;
    ctrl_puts("[controller] Proxying AgentFS GET for worker...\n");

    /* Extract object ID from request data */
    uint8_t get_payload[20];
    get_payload[0] = (uint8_t)(OP_AGENTFS_GET);
    get_payload[1] = 0u; get_payload[2] = 0u; get_payload[3] = 0u;
    /* Copy 16 bytes of object ID from req->data */
    for (uint32_t i = 0; i < 16u && i < req->length; i++)
        get_payload[4 + i] = req->data[i];
    for (uint32_t i = req->length; i < 16u; i++)
        get_payload[4 + i] = 0u;

    sel4_msg_t agentfs_rep;
    uint32_t rc = sel4_client_call(g_ep_agentfs, OP_AGENTFS_GET,
                                   get_payload, sizeof(get_payload), &agentfs_rep);
    if (rc == SEL4_ERR_OK) {
        uint32_t version = (uint32_t)agentfs_rep.data[0];
        uint32_t size    = (uint32_t)agentfs_rep.data[4] | ((uint32_t)agentfs_rep.data[5] << 8)
                         | ((uint32_t)agentfs_rep.data[6] << 16) | ((uint32_t)agentfs_rep.data[7] << 24);
        uint32_t cap_tag = (uint32_t)agentfs_rep.data[8];
        ctrl_puts("[controller] AgentFS returned object\n");
        rep->data[0] = 0u;   /* status OK */
        rep->data[1] = (uint8_t)(size & 0xff);
        rep->data[2] = (uint8_t)((size >> 8) & 0xff);
        rep->data[3] = (uint8_t)((size >> 16) & 0xff);
        rep->data[4] = (uint8_t)((size >> 24) & 0xff);
        rep->data[5] = (uint8_t)(cap_tag & 0xff);
        rep->data[6] = (uint8_t)(version & 0xff);
        rep->length  = 7u;
        return SEL4_ERR_OK;
    } else {
        ctrl_puts("[controller] AgentFS GET failed\n");
        rep->data[0] = (uint8_t)(rc & 0xff);
        rep->length  = 1u;
        return rc;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Worker-pool notification handler (called from notification loop)
 * ─────────────────────────────────────────────────────────────────────────── */

static void on_worker_complete(uint32_t pool_slot)
{
    if (pool_slot == 0 && ctrl.worker_task_dispatched && !ctrl.demo_complete) {
        ctrl.demo_complete = true;
        ctrl_puts("[controller] Worker 0 task COMPLETE\n");

        /* Publish TASK_COMPLETE event */
        ctrl_puts("[controller] Publishing TASK_COMPLETE event to EventBus...\n");
        uint8_t evt[4];
        evt[0] = (uint8_t)(MSG_EVENT_AGENT_EXITED & 0xff);
        evt[1] = (uint8_t)((MSG_EVENT_AGENT_EXITED >> 8) & 0xff);
        evt[2] = 0u; evt[3] = 0u;
        sel4_msg_t rep;
        sel4_client_call(g_ep_eventbus, (uint32_t)MSG_EVENT_AGENT_EXITED,
                         evt, sizeof(evt), &rep);
        ctrl_puts("[controller] TASK_COMPLETE event published\n");

        /* Signal initagent to query EventBus status */
        seL4_Signal(g_initagent_ntfn_cap);

        demo_delay();

        ctrl_puts("\n"
                  "------------------------------------------------------\n"
                  "  Steps 1-3 complete: AgentFS + EventBus + Workers\n"
                  "------------------------------------------------------\n\n");

        /* Step 4: VibeEngine hot-swap demo (direct path) */
        ctrl_puts("[controller] Step 4: VibeEngine hot-swap demo...\n");
        ctrl_puts("[controller] Direct path: loading echo_service.wasm into swap slot 0\n");

        int demo_manifest_ok = verify_capabilities_manifest(
            ECHO_SERVICE_WASM, ECHO_SERVICE_WASM_LEN);
        if (demo_manifest_ok == -2) {
            ctrl_puts("[monitor] WASM manifest hash mismatch — rejecting agent load\n");
            return;
        } else if (demo_manifest_ok == -1) {
            ctrl_puts("[monitor] no capability manifest — granting minimal defaults only\n");
        }

        ctrl.vibe_demo_triggered = true;
        int vslot = vibe_swap_begin(2, ECHO_SERVICE_WASM, ECHO_SERVICE_WASM_LEN);
        if (vslot < 0) {
            ctrl_puts("[controller] Step 4 vibe_swap_begin FAILED\n");
            ctrl.vibe_demo_triggered = false;
        } else {
            ctrl_puts("[controller] Step 4: WASM loaded into swap slot\n");
            ctrl.vibe_swap_in_progress = true;
        }
    } else {
        char s[2] = { (char)('0' + (pool_slot % 10)), '\0' };
        ctrl_puts("[controller] Worker ");
        ctrl_puts(s);
        ctrl_puts(" ready\n");
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Notification loop — called periodically or on notification arrival.
 * In raw seL4 this would be driven by a separate notification recv path.
 * Here we expose it as a callable function for testing.
 * ─────────────────────────────────────────────────────────────────────────── */

void controller_handle_notification(uint32_t notif_kind, uint32_t arg0, uint32_t arg1)
{
    ctrl.notification_count++;

    /* lwIP timer tick */
    ctrl.net_tick_counter++;
    if (ctrl.net_tick_counter >= NET_TICK_INTERVAL) {
        ctrl.net_tick_counter = 0;
        seL4_Signal(g_net_timer_ntfn_cap);
    }

    switch (notif_kind) {
        case 0:  /* EventBus notification */
            ctrl_puts("[controller] EventBus notification\n");
            ctrl.eventbus_ready = true;
            break;

        case 1:  /* InitAgent notification */
            if (arg0 == (uint32_t)MSG_SPAWN_AGENT) {
                /*
                 * init_agent relay: spawn a WASM agent.
                 * arg1 = spawn_id (simplified: full hash in prod).
                 */
                uint32_t spawn_id = arg1;
                ctrl_puts("[controller] SPAWN_AGENT request: spawn_id=");
                char spbuf[12]; uint32_to_dec(spawn_id, spbuf, sizeof(spbuf));
                ctrl_puts(spbuf); ctrl_puts("\n");

                char agent_name[17] = "wasm-agent-00000";
                uint32_t sid = spawn_id;
                for (int ni = 15; ni >= 11; ni--) {
                    agent_name[ni] = (char)('0' + (sid % 10));
                    sid /= 10;
                }

                uint8_t spawn_payload[8];
                spawn_payload[0] = (uint8_t)(spawn_id & 0xff);
                spawn_payload[1] = (uint8_t)((spawn_id >> 8) & 0xff);
                spawn_payload[2] = (uint8_t)((spawn_id >> 16) & 0xff);
                spawn_payload[3] = (uint8_t)((spawn_id >> 24) & 0xff);
                spawn_payload[4] = 0u; spawn_payload[5] = 0u;
                spawn_payload[6] = 0u; spawn_payload[7] = 0u;

                int slot = agent_pool_spawn(agent_name, 0,
                                            spawn_payload, 8u, 80u);

                /* Signal initagent with result */
                seL4_Signal(g_initagent_ntfn_cap);

                if (slot >= 0) {
                    ctrl_puts("[controller] Agent spawned: slot=");
                    char s[2] = { (char)('0' + (slot % 10)), '\0' };
                    ctrl_puts(s); ctrl_puts("\n");
                } else {
                    ctrl_puts("[controller] SPAWN_AGENT: pool exhausted\n");
                }
            } else {
                ctrl_puts("[controller] InitAgent ready notification received\n");
                ctrl.initagent_ready = true;
            }
            break;

        case 2:  /* Quota revoke notification */
            if (arg0 == (uint32_t)MSG_QUOTA_REVOKE) {
                uint32_t agent_id = arg1;
                ctrl_puts("[controller] Quota revoke request: agent=");
                char abuf[12]; uint32_to_dec(agent_id, abuf, sizeof(abuf));
                ctrl_puts(abuf); ctrl_puts("\n");
                cap_broker_revoke_agent(agent_id, 0);
            } else {
                ctrl_puts("[controller] Unknown quota notify\n");
            }
            break;

        case 3:  /* VibeEngine notification — swap approved */
            vibe_demo_step4_notify();
            break;

        case 4:  /* Swap slot health notification */
        {
            uint32_t swap_slot_idx = arg1;
            if (arg0 == 0u) {
                ctrl_puts("[controller] Swap slot health OK — activating\n");
                vibe_swap_health_notify((int)swap_slot_idx);
                if (ctrl.vibe_swap_in_progress && !ctrl.vibe_demo_complete) {
                    ctrl.vibe_swap_in_progress = false;
                    ctrl.vibe_demo_complete = true;
                    ctrl_puts("\n"
                              "------------------------------------------------------\n"
                              "  Steps 1-4 complete — launching microservice demo...\n"
                              "------------------------------------------------------\n\n");
                    microservice_demo();
                }
            } else {
                ctrl_puts("[controller] Swap slot health FAIL\n");
            }
        }
        break;

        case 5:  /* GPU scheduler dispatch */
            if (arg0 == (uint32_t)MSG_GPU_SUBMIT) {
                uint32_t slot_id = arg1;
                ctrl_puts("[controller] GPU task dispatched to slot=");
                char s[2] = { (char)('0' + (slot_id % 10)), '\0' };
                ctrl_puts(s); ctrl_puts("\n");
                if (slot_id < (uint32_t)NUM_SWAP_SLOTS)
                    seL4_Signal(g_initagent_ntfn_cap);  /* proxy to swap slot */
            } else {
                ctrl_puts("[controller] GPU Scheduler online\n");
            }
            break;

        case 6:  /* Mesh agent notification */
            ctrl_puts("[controller] Distributed mesh agent online\n");
            break;

        default:
            /* Worker pool slots 10-17 */
            if (notif_kind >= 10u && notif_kind <= 17u) {
                on_worker_complete(notif_kind - 10u);
            } else {
                ctrl_puts("[controller] Unknown notification kind\n");
            }
            break;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * controller_main — raw seL4 entry point
 *
 * Parameters:
 *   my_ep  — this PD's inbound endpoint capability (from root-task boot info)
 *   ns_ep  — nameserver endpoint capability
 * ─────────────────────────────────────────────────────────────────────────── */

void controller_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    agentos_log_boot("controller");

    /* ── 1. Initialise client ───────────────────────────────────────────────── */
    sel4_client_init(&g_client, ns_ep, 0 /* my_cnode */, 0x100u /* first_free_slot */);

    /* Zero controller state */
    ctrl.eventbus_ready        = false;
    ctrl.initagent_ready       = false;
    ctrl.notification_count    = 0;
    ctrl.demo_obj_stored       = false;
    ctrl.worker_task_dispatched = false;
    ctrl.demo_complete         = false;
    ctrl.vibe_demo_triggered   = false;
    ctrl.vibe_swap_in_progress = false;
    ctrl.vibe_demo_complete    = false;
    ctrl.net_tick_counter      = 0;

    ctrl_puts("[controller] Initializing agentOS core services\n");

    /* ── 2. Apply capability policy ─────────────────────────────────────────── */
    monitor_apply_policy();

    /* ── 3. Initialise subsystems ───────────────────────────────────────────── */
    cap_broker_init();
    agent_pool_init();
    boot_integrity_init();

    /* ── 4. Ed25519 selftest ────────────────────────────────────────────────── */
    {
        static const uint8_t test_sk[32] = {
            0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
            0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0x44,
            0xda, 0x4d, 0xa0, 0x5d, 0xe7, 0xe8, 0xc8, 0x6b,
            0xef, 0x64, 0x77, 0x64, 0xb4, 0x24, 0x09, 0x57
        };
        static const uint8_t test_msg[1] = {0};
        uint8_t pk[32], sig[64];
        crypto_ed25519_public_key(pk, test_sk);
        crypto_ed25519_sign(sig, test_sk, pk, test_msg, 0);
        bool ok = crypto_ed25519_check(sig, test_msg, 0, pk) == 0;
        ctrl_puts(ok ? "[verify] Ed25519 selftest PASS\n"
                     : "[verify] Ed25519 selftest FAIL\n");
    }

    /* ── 5. Connect to upstream services ────────────────────────────────────── */
    sel4_client_connect(&g_client, NS_SVC_EVENTBUS,   &g_ep_eventbus);
    sel4_client_connect(&g_client, NS_SVC_AGENTFS,    &g_ep_agentfs);
    sel4_client_connect(&g_client, NS_SVC_VFS,        &g_ep_vfs);
    sel4_client_connect(&g_client, NS_SVC_NET,        &g_ep_net);
    sel4_client_connect(&g_client, NS_SVC_SPAWN,      &g_ep_spawn);
    sel4_client_connect(&g_client, NS_SVC_APPMANAGER, &g_ep_appmgr);
    sel4_client_connect(&g_client, NS_SVC_HTTP,       &g_ep_http);

    /* Obtain notification caps from nameserver.
     * In real hardware the nameserver mints badged ntfn caps;
     * in host tests these remain 0 and seL4_Signal is a no-op. */
    sel4_client_connect(&g_client, "event_bus_ntfn",   &g_event_bus_ntfn_cap);
    sel4_client_connect(&g_client, "initagent_ntfn",   &g_initagent_ntfn_cap);
    sel4_client_connect(&g_client, "net_timer_ntfn",   &g_net_timer_ntfn_cap);

    /* ── 6. Wake EventBus via service call ──────────────────────────────────── */
    ctrl_puts("[controller] Waking EventBus via IPC call...\n");
    {
        sel4_msg_t rep;
        uint32_t rc = sel4_client_call(g_ep_eventbus, (uint32_t)MSG_EVENTBUS_INIT,
                                       (void *)0, 0u, &rep);
        if (rc == SEL4_ERR_OK) {
            ctrl.eventbus_ready = true;
            ctrl_puts("[controller] EventBus: READY\n");
        } else {
            ctrl_puts("[controller] EventBus: unexpected response\n");
        }
    }

    /* ── 7. Signal InitAgent to start ───────────────────────────────────────── */
    ctrl_puts("[controller] Notifying InitAgent to start...\n");
    seL4_Signal(g_initagent_ntfn_cap);

    /* ── 8. Initialise vibe-swap subsystem ──────────────────────────────────── */
    vibe_swap_init();

    /* ── 9. Boot complete ───────────────────────────────────────────────────── */
    ctrl_puts("[controller] *** agentOS controller boot complete ***\n");
    ctrl_puts("[controller] Ready for agents.\n");
    /* Canonical boot-complete marker — must match xtask/cmd_test.rs */
    ctrl_puts("agentOS boot complete\n");

    /* ── 10. Register services with NameServer ──────────────────────────────── */
    ctrl_puts("[controller] Registering services with NameServer...\n");
    ns_register_service(NS_SVC_VFS,        19u  /* CH_VFS_SERVER   */, TRACE_PD_VFS_SERVER,   CAP_CLASS_FS);
    ns_register_service(NS_SVC_SPAWN,      20u  /* CH_SPAWN_SERVER */, TRACE_PD_SPAWN_SERVER, CAP_CLASS_SPAWN);
    ns_register_service(NS_SVC_NET,        21u  /* CH_NET_SERVER   */, TRACE_PD_NET_SERVER,   CAP_CLASS_NET);
    ns_register_service("virtio_blk",      22u  /* CH_VIRTIO_BLK   */, TRACE_PD_VIRTIO_BLK,   CAP_CLASS_FS);
    ns_register_service(NS_SVC_APPMANAGER, 23u  /* CH_APP_MANAGER  */, TRACE_PD_APP_MANAGER,  CAP_CLASS_SPAWN | CAP_CLASS_NET);
    ns_register_service(NS_SVC_HTTP,       24u  /* CH_HTTP_SVC     */, TRACE_PD_HTTP_SVC,     CAP_CLASS_NET);
    ctrl_puts("[controller] 6 microkernel services registered\n");

    /* ── 11. Legacy data-flow demo (Steps 1-4) ──────────────────────────────── */
    demo_sequence();

    /* ── 12. Register self as "controller" with nameserver ──────────────────── */
    ns_register_service("controller", 0u, TRACE_PD_CONTROLLER, 0u);

    /* ── 13. Register inbound handlers and enter dispatch loop ─────────────── */
    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, (uint32_t)OP_CAP_POLICY_RELOAD, handle_cap_policy_reload, (void *)0);
    sel4_server_register(&g_srv, (uint32_t)MSG_VMM_VCPU_SET_REGS, handle_vmm_vcpu_set_regs, (void *)0);
    sel4_server_register(&g_srv, (uint32_t)MSG_VMM_REGISTER,      handle_vmm_register,      (void *)0);
    sel4_server_register(&g_srv, (uint32_t)MSG_WORKER_RETRIEVE,   handle_worker_retrieve,    (void *)0);

#ifndef AGENTOS_TEST_HOST
    /* Enter the never-returning server dispatch loop */
    sel4_server_run(&g_srv);
#endif
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test-only exported symbols
 * ─────────────────────────────────────────────────────────────────────────── */
#ifdef AGENTOS_TEST_HOST

/* Allow test code to invoke the server dispatch directly */
uint32_t controller_dispatch_one(sel4_badge_t badge,
                                  const sel4_msg_t *req,
                                  sel4_msg_t *rep)
{
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

/* Expose ctrl state accessors for test assertions */
bool controller_eventbus_ready(void)   { return ctrl.eventbus_ready; }
bool controller_initagent_ready(void)  { return ctrl.initagent_ready; }
uint32_t controller_notif_count(void)  { return ctrl.notification_count; }
bool controller_demo_obj_stored(void)  { return ctrl.demo_obj_stored; }
bool controller_worker_dispatched(void){ return ctrl.worker_task_dispatched; }
bool controller_demo_complete(void)    { return ctrl.demo_complete; }
bool controller_vibe_triggered(void)   { return ctrl.vibe_demo_triggered; }
bool controller_vibe_in_progress(void) { return ctrl.vibe_swap_in_progress; }
bool controller_vibe_complete(void)    { return ctrl.vibe_demo_complete; }
bool controller_policy_loaded(void)    { return g_policy_loaded; }

/* Expose g_srv handler_count so tests can verify registration */
uint32_t controller_handler_count(void) { return g_srv.handler_count; }

#endif /* AGENTOS_TEST_HOST */
