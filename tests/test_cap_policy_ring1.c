/*
 * agentOS Cap-Policy Ring-1 Enforcement — Unit Tests
 *
 * Tests the ring-1 guest IPC enforcement logic:
 *   cap_policy_is_ring0_channel()  — channel classification
 *   cap_policy_guest_ipc_check()   — VMM IPC gate (reject ring-0 access)
 *   cap_policy_vcpu_el_check()     — vCPU privilege level (EL2/CPL0 rejection)
 *
 * Build:  cc -o /tmp/test_cap_policy_ring1 \
 *             tests/test_cap_policy_ring1.c \
 *             -I tests \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST
 * Run:    /tmp/test_cap_policy_ring1
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Host-side stubs (microkit.h replacement for host builds)
 * ══════════════════════════════════════════════════════════════════════════ */
#ifdef AGENTOS_TEST_HOST

static inline void microkit_dbg_puts(const char *s) { (void)s; }

/* Minimal channel ID definitions (mirrors agentos.h) */
#define CH_SERIAL_PD   67u
#define CH_NET_PD      68u
#define CH_BLOCK_PD    69u
#define CH_USB_PD      70u
#define CH_FB_PD       71u
#define CH_GUEST_PD    75u
#define CH_VMM_KERNEL  76u

/* Sample ring-0 channels that guests must not access */
#define CH_AGENTFS      5u
#define CH_TRACE_CTRL   6u
#define CH_NAMESERVER  18u
#define CH_VFS_SERVER  19u
#define CH_SPAWN_SERVER 20u
#define CH_NET_SERVER  21u
#define CH_VIRTIO_BLK  22u
#define CH_APP_MANAGER 23u
#define CH_HTTP_SVC    24u
#define CH_VIBEENGINE  40u
#define CH_VM_MANAGER  45u
#define CH_QUOTA_CTRL  52u
#define CH_WATCHDOG_CTRL 56u
#define CH_CAP_AUDIT_CTRL 57u
#define CH_GPU_SHMEM   61u
#define CH_DEBUG_BRIDGE 62u

#endif /* AGENTOS_TEST_HOST */

/* ══════════════════════════════════════════════════════════════════════════
 * Inline copy of the ring-1 enforcement logic from cap_policy.c
 * ══════════════════════════════════════════════════════════════════════════ */

#define TRACE_PD_LINUX_VMM    41u
#define TRACE_PD_FREEBSD_VMM  42u

static const uint32_t g_guest_allowed_ch[] = {
    CH_SERIAL_PD,
    CH_NET_PD,
    CH_BLOCK_PD,
    CH_USB_PD,
    CH_FB_PD,
    CH_GUEST_PD,
    CH_VMM_KERNEL,
};

#define GUEST_ALLOWED_CH_N \
    ((uint32_t)(sizeof(g_guest_allowed_ch) / sizeof(g_guest_allowed_ch[0])))

static int cap_policy_is_ring0_channel(uint32_t channel_id)
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

static int cap_policy_guest_ipc_check(uint32_t caller_pd_id, uint32_t target_channel)
{
    if (!is_vmm_pd(caller_pd_id))
        return 0;
    if (cap_policy_is_ring0_channel(target_channel))
        return -1;
    return 0;
}

#define SPSR_M_MASK  0xFu
#define SPSR_EL2t    0x8u
#define SPSR_EL2h    0x9u
#define X86_CPL_MASK 0x3u
#define X86_CPL3     0x3u

static int cap_policy_vcpu_el_check(uint64_t spsr, bool is_aarch64)
{
    if (is_aarch64) {
        uint32_t m = (uint32_t)(spsr & SPSR_M_MASK);
        return (m == SPSR_EL2t || m == SPSR_EL2h) ? -1 : 0;
    }
    /* x86: CS.RPL must be exactly CPL3; CPL0/1/2 are all forbidden */
    return ((spsr & X86_CPL_MASK) != X86_CPL3) ? -1 : 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test framework
 * ══════════════════════════════════════════════════════════════════════════ */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)       printf("\n=== TEST: %s ===\n", (name))
#define ASSERT_TRUE(expr, msg)  do { \
    if (!(expr)) { printf("  FAIL: %s\n", (msg)); tests_failed++; } \
    else         { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)
#define ASSERT_EQ(a, b, msg)  do { \
    if ((int64_t)(a) != (int64_t)(b)) { \
        printf("  FAIL: %s (got %lld expected %lld)\n", (msg), \
               (long long)(a), (long long)(b)); tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: cap_policy_is_ring0_channel
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_guest_allowed_channels_pass(void)
{
    TEST("guest_allowed_channels_not_ring0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_SERIAL_PD),  0, "CH_SERIAL_PD is allowed");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_NET_PD),     0, "CH_NET_PD is allowed");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_BLOCK_PD),   0, "CH_BLOCK_PD is allowed");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_USB_PD),     0, "CH_USB_PD is allowed");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_FB_PD),      0, "CH_FB_PD is allowed");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_GUEST_PD),   0, "CH_GUEST_PD is allowed");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_VMM_KERNEL), 0, "CH_VMM_KERNEL is allowed");
}

static void test_ring0_channels_rejected(void)
{
    TEST("ring0_channels_are_ring0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_AGENTFS),       1, "CH_AGENTFS is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_TRACE_CTRL),    1, "CH_TRACE_CTRL is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_NAMESERVER),    1, "CH_NAMESERVER is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_VFS_SERVER),    1, "CH_VFS_SERVER is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_SPAWN_SERVER),  1, "CH_SPAWN_SERVER is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_NET_SERVER),    1, "CH_NET_SERVER is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_VIRTIO_BLK),    1, "CH_VIRTIO_BLK is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_APP_MANAGER),   1, "CH_APP_MANAGER is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_HTTP_SVC),      1, "CH_HTTP_SVC is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_VIBEENGINE),    1, "CH_VIBEENGINE is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_VM_MANAGER),    1, "CH_VM_MANAGER is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_QUOTA_CTRL),    1, "CH_QUOTA_CTRL is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_WATCHDOG_CTRL), 1, "CH_WATCHDOG_CTRL is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_CAP_AUDIT_CTRL),1, "CH_CAP_AUDIT_CTRL is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_GPU_SHMEM),     1, "CH_GPU_SHMEM is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(CH_DEBUG_BRIDGE),  1, "CH_DEBUG_BRIDGE is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(0u),               1, "channel 0 is ring-0");
    ASSERT_EQ(cap_policy_is_ring0_channel(255u),             1, "unknown channel is ring-0");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: cap_policy_guest_ipc_check
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_vmm_to_device_pd_allowed(void)
{
    TEST("vmm_to_device_pd_allowed");
    /* linux_vmm → device PDs: all allowed */
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_SERIAL_PD),  0,
              "linux_vmm → serial_pd allowed");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_NET_PD),     0,
              "linux_vmm → net_pd allowed");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_BLOCK_PD),   0,
              "linux_vmm → block_pd allowed");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_USB_PD),     0,
              "linux_vmm → usb_pd allowed");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_FB_PD),      0,
              "linux_vmm → fb_pd allowed");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_GUEST_PD),   0,
              "linux_vmm → guest_pd allowed");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_VMM_KERNEL), 0,
              "linux_vmm → vmm_kernel allowed");

    /* freebsd_vmm → device PDs: all allowed */
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_FREEBSD_VMM, CH_SERIAL_PD), 0,
              "freebsd_vmm → serial_pd allowed");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_FREEBSD_VMM, CH_BLOCK_PD),  0,
              "freebsd_vmm → block_pd allowed");
}

static void test_vmm_to_ring0_rejected(void)
{
    TEST("vmm_to_ring0_channel_rejected");
    /* linux_vmm → ring-0 channels: all EPERM */
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_AGENTFS),       -1,
              "linux_vmm → agentfs REJECTED");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_NAMESERVER),    -1,
              "linux_vmm → nameserver REJECTED");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_VFS_SERVER),    -1,
              "linux_vmm → vfs_server REJECTED");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_SPAWN_SERVER),  -1,
              "linux_vmm → spawn_server REJECTED");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_NET_SERVER),    -1,
              "linux_vmm → net_server REJECTED");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_VIBEENGINE),    -1,
              "linux_vmm → vibe_engine REJECTED");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_VM_MANAGER),    -1,
              "linux_vmm → vm_manager REJECTED");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_QUOTA_CTRL),    -1,
              "linux_vmm → quota_pd REJECTED");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_CAP_AUDIT_CTRL),-1,
              "linux_vmm → cap_audit REJECTED");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_WATCHDOG_CTRL), -1,
              "linux_vmm → watchdog REJECTED");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_LINUX_VMM, CH_GPU_SHMEM),     -1,
              "linux_vmm → gpu_shmem REJECTED");

    /* freebsd_vmm → ring-0 channels: all EPERM */
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_FREEBSD_VMM, CH_AGENTFS),    -1,
              "freebsd_vmm → agentfs REJECTED");
    ASSERT_EQ(cap_policy_guest_ipc_check(TRACE_PD_FREEBSD_VMM, CH_VIBEENGINE), -1,
              "freebsd_vmm → vibe_engine REJECTED");
}

static void test_non_vmm_pd_unrestricted(void)
{
    TEST("non_vmm_pd_unrestricted");
    /* Non-VMM PDs are not subject to guest ring-1 enforcement */
    uint32_t system_pd = 0u;
    uint32_t worker_pd = 3u;
    uint32_t agentfs_pd = 11u;

    ASSERT_EQ(cap_policy_guest_ipc_check(system_pd,  CH_AGENTFS),   0,
              "system pd → agentfs allowed (not a VMM)");
    ASSERT_EQ(cap_policy_guest_ipc_check(worker_pd,  CH_VIBEENGINE),0,
              "worker pd → vibeengine allowed (not a VMM)");
    ASSERT_EQ(cap_policy_guest_ipc_check(agentfs_pd, CH_VM_MANAGER),0,
              "agentfs pd → vm_manager allowed (not a VMM)");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: cap_policy_vcpu_el_check — AArch64
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_aarch64_el1_allowed(void)
{
    TEST("aarch64_el1_allowed");
    /* EL1h (0x5) — guest OS kernel mode, normal */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x5u, true),  0, "SPSR EL1h (0x5) allowed");
    /* EL1t (0x4) — EL1 with SP_EL0 */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x4u, true),  0, "SPSR EL1t (0x4) allowed");
    /* EL0t (0x0) — guest user mode */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x0u, true),  0, "SPSR EL0t (0x0) allowed");
    /* EL0t with IRQ mask flags set (high bits irrelevant to M field) */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x3C5u, true), 0, "SPSR EL1h with DAIF masked allowed");
    /* Realistic boot SPSR: EL1h, AIF masked = 0x3C5 */
    ASSERT_EQ(cap_policy_vcpu_el_check(0xFFFFFFFFFFFFFF05ULL, true), 0,
              "SPSR EL1h with all high bits set: still EL1h");
}

static void test_aarch64_el2_rejected(void)
{
    TEST("aarch64_el2_rejected");
    /* EL2t (0x8) — hypervisor EL2 with SP_EL0: MUST be rejected */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x8u, true), -1, "SPSR EL2t (0x8) REJECTED");
    /* EL2h (0x9) — hypervisor EL2 with SP_EL2: MUST be rejected */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x9u, true), -1, "SPSR EL2h (0x9) REJECTED");
    /* EL2t with DAIF flags */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x3C8u, true), -1,
              "SPSR EL2t with DAIF flags REJECTED");
    /* EL2h with DAIF flags */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x3C9u, true), -1,
              "SPSR EL2h with DAIF flags REJECTED");
    /* All-ones with EL2t in low nibble */
    ASSERT_EQ(cap_policy_vcpu_el_check(0xFFFFFFFFFFFFFFF8ULL, true), -1,
              "SPSR all-ones with M=EL2t REJECTED");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: cap_policy_vcpu_el_check — x86
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_x86_cpl3_allowed(void)
{
    TEST("x86_cpl3_allowed");
    /* CS.RPL = 3 (user mode / CPL3): allowed */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x3u, false),   0, "x86 CPL3 (CS=0x3) allowed");
    /* CS selector 0x1B = RPL3 (common in Linux/FreeBSD user CS) */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x1Bu, false),  0, "x86 CPL3 (CS=0x1B) allowed");
    /* CS selector 0x23 = RPL3 */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x23u, false),  0, "x86 CPL3 (CS=0x23) allowed");
    /* CS selector 0x2B = RPL3 */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x2Bu, false),  0, "x86 CPL3 (CS=0x2B) allowed");
    /* CS selector 0x33 = RPL3 */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x33u, false),  0, "x86 CPL3 (CS=0x33) allowed");
}

static void test_x86_cpl0_rejected(void)
{
    TEST("x86_cpl0_rejected");
    /* CS.RPL = 0 (kernel mode / CPL0): MUST be rejected */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x0u, false),   -1, "x86 CPL0 (CS=0x0) REJECTED");
    /* CS selector 0x08 (kernel code, RPL=0) */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x8u, false),   -1, "x86 CPL0 (CS=0x08) REJECTED");
    /* CS selector 0x10 (kernel data, RPL=0) */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x10u, false),  -1, "x86 CPL0 (CS=0x10) REJECTED");
    /* CS selector 0x28 (kernel 64-bit, RPL=0) */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x28u, false),  -1, "x86 CPL0 (CS=0x28) REJECTED");
    /* CPL1 (ring 1 — also forbidden) */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x1u, false),   -1, "x86 CPL1 REJECTED");
    /* CPL2 (ring 2 — also forbidden) */
    ASSERT_EQ(cap_policy_vcpu_el_check(0x2u, false),   -1, "x86 CPL2 REJECTED");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  agentOS Cap-Policy Ring-1 Enforcement — Test Suite  ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    /* Channel classification */
    test_guest_allowed_channels_pass();
    test_ring0_channels_rejected();

    /* Guest IPC enforcement */
    test_vmm_to_device_pd_allowed();
    test_vmm_to_ring0_rejected();
    test_non_vmm_pd_unrestricted();

    /* vCPU privilege level — AArch64 */
    test_aarch64_el1_allowed();
    test_aarch64_el2_rejected();

    /* vCPU privilege level — x86 */
    test_x86_cpl3_allowed();
    test_x86_cpl0_rejected();

    printf("\n══════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    if (tests_failed > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
