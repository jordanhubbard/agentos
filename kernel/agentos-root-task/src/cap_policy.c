/*
 * agentOS Capability Policy Engine
 *
 * Defines the policy rules for capability grants and agent resource limits.
 * The policy table maps agent classes to allowed capabilities and resource
 * quotas. Used by cap_broker and init_agent during agent spawn to determine
 * what capabilities to grant and what resource limits to enforce.
 *
 * This is compiled into the controller PD (monitor.elf) alongside cap_broker.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "cap_policy.h"

/* ── Policy Entry ─────────────────────────────────────────────────────────── */

/*
 * PolicyEntry defines the capability set and resource quotas for an agent class.
 *
 * When an agent is spawned, init_agent looks up its class in the policy table
 * to determine:
 *   1. Which capabilities to grant (caps_mask)
 *   2. CPU time quota (cpu_quota_ms) — enforced by quota_pd
 *   3. Memory quota (mem_quota_kb) — enforced by quota_pd
 *   4. Maximum priority level
 */
typedef struct {
    const char *class_name;    /* human-readable class name */
    uint32_t    class_id;      /* numeric class identifier */
    uint32_t    caps_mask;     /* bitmask of CAP_CLASS_* capabilities */
    uint32_t    max_priority;  /* maximum scheduling priority */
    uint32_t    cpu_quota_ms;  /* CPU time limit in ms (0 = unlimited) */
    uint32_t    mem_quota_kb;  /* memory limit in KB (0 = unlimited) */
    bool        allow_spawn;   /* can this agent spawn children? */
    bool        allow_gpu;     /* can this agent use GPU? */
} PolicyEntry;

/* ── Default Policy Table ─────────────────────────────────────────────────── */

/*
 * Built-in policy table. In production, this would be loaded from the
 * policy DSL (policy/default_policy.nano), but for now we hardcode
 * sensible defaults for each agent class.
 */
#define POLICY_TABLE_SIZE 6

static const PolicyEntry policy_table[POLICY_TABLE_SIZE] = {
    {
        .class_name   = "system",
        .class_id     = 0,
        .caps_mask    = CAP_CLASS_FS | CAP_CLASS_NET | CAP_CLASS_GPU |
                        CAP_CLASS_IPC | CAP_CLASS_TIMER | CAP_CLASS_STDIO |
                        CAP_CLASS_SPAWN | CAP_CLASS_SWAP,
        .max_priority = PRIO_SOFT_RT,
        .cpu_quota_ms = 0,        /* unlimited for system agents */
        .mem_quota_kb = 0,        /* unlimited */
        .allow_spawn  = true,
        .allow_gpu    = true,
    },
    {
        .class_name   = "compute",
        .class_id     = 1,
        .caps_mask    = CAP_CLASS_GPU | CAP_CLASS_IPC | CAP_CLASS_TIMER |
                        CAP_CLASS_STDIO,
        .max_priority = PRIO_COMPUTE,
        .cpu_quota_ms = 30000,    /* 30 seconds CPU time */
        .mem_quota_kb = 65536,    /* 64MB */
        .allow_spawn  = false,
        .allow_gpu    = true,
    },
    {
        .class_name   = "interactive",
        .class_id     = 2,
        .caps_mask    = CAP_CLASS_FS | CAP_CLASS_NET | CAP_CLASS_IPC |
                        CAP_CLASS_TIMER | CAP_CLASS_STDIO,
        .max_priority = PRIO_INTERACTIVE,
        .cpu_quota_ms = 10000,    /* 10 seconds CPU time */
        .mem_quota_kb = 16384,    /* 16MB */
        .allow_spawn  = true,
        .allow_gpu    = false,
    },
    {
        .class_name   = "background",
        .class_id     = 3,
        .caps_mask    = CAP_CLASS_IPC | CAP_CLASS_TIMER | CAP_CLASS_STDIO,
        .max_priority = PRIO_BACKGROUND,
        .cpu_quota_ms = 5000,     /* 5 seconds CPU time */
        .mem_quota_kb = 4096,     /* 4MB */
        .allow_spawn  = false,
        .allow_gpu    = false,
    },
    {
        .class_name   = "sandbox",
        .class_id     = 4,
        .caps_mask    = CAP_CLASS_STDIO,
        .max_priority = PRIO_BACKGROUND,
        .cpu_quota_ms = 1000,     /* 1 second CPU time */
        .mem_quota_kb = 1024,     /* 1MB */
        .allow_spawn  = false,
        .allow_gpu    = false,
    },
    {
        .class_name   = "gpu_worker",
        .class_id     = 5,
        .caps_mask    = CAP_CLASS_GPU | CAP_CLASS_IPC | CAP_CLASS_STDIO,
        .max_priority = PRIO_COMPUTE,
        .cpu_quota_ms = 60000,    /* 60 seconds CPU time */
        .mem_quota_kb = 131072,   /* 128MB */
        .allow_spawn  = false,
        .allow_gpu    = true,
    },
};

/* ── Policy lookup ────────────────────────────────────────────────────────── */

/*
 * Look up a policy entry by class ID.
 * Returns NULL if the class is not found (caller should deny).
 */
const PolicyEntry *cap_policy_lookup(uint32_t class_id) {
    for (int i = 0; i < POLICY_TABLE_SIZE; i++) {
        if (policy_table[i].class_id == class_id) {
            return &policy_table[i];
        }
    }
    return (const PolicyEntry *)0;  /* NULL — deny by default */
}

/*
 * Get the default policy for unknown/unclassified agents.
 * Returns the "sandbox" policy (most restrictive).
 */
const PolicyEntry *cap_policy_default(void) {
    return &policy_table[4];  /* sandbox */
}

/*
 * Check if a capability mask is allowed by a policy entry.
 */
bool cap_policy_check(const PolicyEntry *policy, uint32_t requested_caps) {
    if (!policy) return false;
    return (policy->caps_mask & requested_caps) == requested_caps;
}

/* ── Ring-1: guest IPC enforcement ───────────────────────────────────────── */

/*
 * Channels that guest VMM PDs (linux_vmm, freebsd_vmm) are permitted to use.
 * These are the generic OS-neutral device PDs defined in guest_contract.h plus
 * the two VMM protocol channels.  Every other channel is ring-0 for guests.
 */
static const uint32_t g_guest_allowed_ch[] = {
    67u,  /* CH_SERIAL_PD  — serial device PD */
    68u,  /* CH_NET_PD     — network device PD */
    69u,  /* CH_BLOCK_PD   — block device PD */
    70u,  /* CH_USB_PD     — USB device PD */
    71u,  /* CH_FB_PD      — framebuffer device PD */
    75u,  /* CH_GUEST_PD   — guest lifecycle management */
    76u,  /* CH_VMM_KERNEL — VMM-to-root-task internal protocol */
};

#define GUEST_ALLOWED_CH_N \
    ((uint32_t)(sizeof(g_guest_allowed_ch) / sizeof(g_guest_allowed_ch[0])))

int cap_policy_is_ring0_channel(uint32_t channel_id)
{
    for (uint32_t i = 0; i < GUEST_ALLOWED_CH_N; i++) {
        if (g_guest_allowed_ch[i] == channel_id)
            return 0;
    }
    return 1;
}

static int is_vmm_pd(uint32_t pd_id)
{
    return pd_id == TRACE_PD_LINUX_VMM || pd_id == TRACE_PD_FREEBSD_VMM;
}

int cap_policy_guest_ipc_check(uint32_t caller_pd_id, uint32_t target_channel)
{
    if (!is_vmm_pd(caller_pd_id))
        return 0;
    if (cap_policy_is_ring0_channel(target_channel))
        return -1;
    return 0;
}

/* AArch64 SPSR.M[3:0] EL encoding */
#define SPSR_M_MASK   0xFu
#define SPSR_EL2t     0x8u   /* EL2 with SP_EL0 — forbidden for guests */
#define SPSR_EL2h     0x9u   /* EL2 with SP_EL2 — forbidden for guests */

/* x86 CS.RPL lives in bits[1:0] of the CS descriptor */
#define X86_CPL_MASK  0x3u
#define X86_CPL3      0x3u   /* user mode — only permitted value for guest vCPUs */

int cap_policy_vcpu_el_check(uint64_t spsr, bool is_aarch64)
{
    if (is_aarch64) {
        uint32_t m = (uint32_t)(spsr & SPSR_M_MASK);
        return (m == SPSR_EL2t || m == SPSR_EL2h) ? -1 : 0;
    }
    /* x86: CS.RPL must be exactly CPL3 (user); CPL0/1/2 are all forbidden */
    return ((spsr & X86_CPL_MASK) != X86_CPL3) ? -1 : 0;
}

/* ── Ring-0 service non-reinvention registry ─────────────────────────────── */

typedef struct {
    uint32_t pd_handle;
    uint32_t channel_id;
    bool     registered;
} ring0_svc_entry_t;

/* Index 0 unused; valid func_class values are 1..CAP_POLICY_FUNC_CLASS_MAX */
static ring0_svc_entry_t g_ring0_registry[CAP_POLICY_FUNC_CLASS_MAX + 1];

int cap_policy_register_ring0_service(uint32_t func_class, uint32_t pd_handle, uint32_t channel_id)
{
    if (func_class < 1 || func_class > CAP_POLICY_FUNC_CLASS_MAX)
        return -1;
    if (g_ring0_registry[func_class].registered)
        return -1;
    g_ring0_registry[func_class].pd_handle  = pd_handle;
    g_ring0_registry[func_class].channel_id = channel_id;
    g_ring0_registry[func_class].registered = true;
    return 0;
}

int cap_policy_find_ring0_service(uint32_t func_class, uint32_t *out_pd_handle, uint32_t *out_channel_id)
{
    if (func_class < 1 || func_class > CAP_POLICY_FUNC_CLASS_MAX)
        return 0;
    if (!g_ring0_registry[func_class].registered)
        return 0;
    if (out_pd_handle)  *out_pd_handle  = g_ring0_registry[func_class].pd_handle;
    if (out_channel_id) *out_channel_id = g_ring0_registry[func_class].channel_id;
    return 1;
}

void cap_policy_unregister_ring0_service(uint32_t func_class)
{
    if (func_class < 1 || func_class > CAP_POLICY_FUNC_CLASS_MAX)
        return;
    g_ring0_registry[func_class].pd_handle  = 0;
    g_ring0_registry[func_class].channel_id = 0;
    g_ring0_registry[func_class].registered = false;
}

/*
 * Initialize the policy engine. Currently just logs readiness.
 * In the future, this would load policy/default_policy.nano.
 */
void cap_policy_init(void) {
    log_drain_write(11, 11, "[cap_policy] Policy table loaded: ");
    /* Print count */
    char buf[4];
    buf[0] = '0' + POLICY_TABLE_SIZE;
    buf[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = buf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " agent classes defined\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(11, 11, _cl_buf);
    }

    for (int i = 0; i < POLICY_TABLE_SIZE; i++) {
        {
            char _cl_buf[256] = {};
            char *_cl_p = _cl_buf;
            for (const char *_s = "[cap_policy]   class="; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = policy_table[i].class_name; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = " cpu="; *_s; _s++) *_cl_p++ = *_s;
            *_cl_p = 0;
            log_drain_write(11, 11, _cl_buf);
        }
        /* Simple decimal for quota values */
        uint32_t v = policy_table[i].cpu_quota_ms;
        if (v == 0) {
            log_drain_write(11, 11, "unlimited");
        } else {
            char qbuf[12];
            int j = 11;
            qbuf[j] = '\0';
            while (v > 0 && j > 0) { qbuf[--j] = '0' + (v % 10); v /= 10; }
            {
                char _cl_buf[256] = {};
                char *_cl_p = _cl_buf;
                for (const char *_s = &qbuf[j]; *_s; _s++) *_cl_p++ = *_s;
                for (const char *_s = "ms"; *_s; _s++) *_cl_p++ = *_s;
                *_cl_p = 0;
                log_drain_write(11, 11, _cl_buf);
            }
        }
        log_drain_write(11, 11, " mem=");
        v = policy_table[i].mem_quota_kb;
        if (v == 0) {
            log_drain_write(11, 11, "unlimited");
        } else {
            char qbuf[12];
            int j = 11;
            qbuf[j] = '\0';
            while (v > 0 && j > 0) { qbuf[--j] = '0' + (v % 10); v /= 10; }
            {
                char _cl_buf[256] = {};
                char *_cl_p = _cl_buf;
                for (const char *_s = &qbuf[j]; *_s; _s++) *_cl_p++ = *_s;
                for (const char *_s = "kb"; *_s; _s++) *_cl_p++ = *_s;
                *_cl_p = 0;
                log_drain_write(11, 11, _cl_buf);
            }
        }
        log_drain_write(11, 11, "\n");
    }
}
