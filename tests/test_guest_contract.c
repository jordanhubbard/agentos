/*
 * agentOS guest_contract + vmm_contract — Contract Unit Tests
 *
 * Validates all MSG_GUEST_* and MSG_VMM_* struct layouts, constants, and
 * error codes without seL4 or Microkit.  No implementation is tested here —
 * the tests verify the API definitions are internally consistent and that
 * the two-layer protocol (Layer A lifecycle + Layer B binding) compiles cleanly.
 *
 * Build:  cc -o /tmp/test_guest_contract \
 *             tests/test_guest_contract.c \
 *             -I tests \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST
 * Run:    /tmp/test_guest_contract
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Headers under test (tests/microkit.h stub satisfies <microkit.h> via -I tests)
 * ══════════════════════════════════════════════════════════════════════════ */

#include "contracts/guest_contract.h"
#include "contracts/vmm_contract.h"

/* ══════════════════════════════════════════════════════════════════════════
 * Test infrastructure
 * ══════════════════════════════════════════════════════════════════════════ */

#define PASS(name)  do { printf("  PASS  %s\n", name); return 0; } while(0)
#define FAIL(msg)   do { printf("  FAIL  %s:%d: %s\n", __FILE__, __LINE__, msg); return 1; } while(0)
#define CHECK(cond) do { if (!(cond)) FAIL(#cond); } while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * Opcode constant tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_guest_opcodes(void)
{
    /* All MSG_GUEST_* must be in the 0x2A00 range and sequential */
    CHECK(MSG_GUEST_CREATE      == 0x2A01u);
    CHECK(MSG_GUEST_BIND_DEVICE == 0x2A02u);
    CHECK(MSG_GUEST_SET_MEMORY  == 0x2A03u);
    CHECK(MSG_GUEST_BOOT        == 0x2A04u);
    CHECK(MSG_GUEST_SUSPEND     == 0x2A05u);
    CHECK(MSG_GUEST_RESUME      == 0x2A06u);
    CHECK(MSG_GUEST_DESTROY     == 0x2A07u);

    /* All MSG_VMM_* must be in the 0x2B00 range */
    CHECK(MSG_VMM_REGISTER      == 0x2B01u);
    CHECK(MSG_VMM_ALLOC_GUEST_MEM == 0x2B02u);
    CHECK(MSG_VMM_VCPU_CREATE   == 0x2B03u);
    CHECK(MSG_VMM_VCPU_DESTROY  == 0x2B04u);
    CHECK(MSG_VMM_VCPU_SET_REGS == 0x2B05u);
    CHECK(MSG_VMM_VCPU_GET_REGS == 0x2B06u);
    CHECK(MSG_VMM_INJECT_IRQ    == 0x2B07u);

    /* Opcode ranges must not overlap */
    CHECK(MSG_GUEST_DESTROY < MSG_VMM_REGISTER);

    PASS("test_guest_opcodes");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Device flag tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_device_flags(void)
{
    /* Flags must be power-of-two and non-overlapping */
    CHECK(GUEST_DEV_FLAG_SERIAL == (1u << 0));
    CHECK(GUEST_DEV_FLAG_NET    == (1u << 1));
    CHECK(GUEST_DEV_FLAG_BLOCK  == (1u << 2));
    CHECK(GUEST_DEV_FLAG_USB    == (1u << 3));
    CHECK(GUEST_DEV_FLAG_FB     == (1u << 4));

    /* GUEST_DEV_* indices match flag bit positions */
    CHECK(GUEST_DEV_FLAG_SERIAL == (1u << GUEST_DEV_SERIAL));
    CHECK(GUEST_DEV_FLAG_NET    == (1u << GUEST_DEV_NET));
    CHECK(GUEST_DEV_FLAG_BLOCK  == (1u << GUEST_DEV_BLOCK));
    CHECK(GUEST_DEV_FLAG_USB    == (1u << GUEST_DEV_USB));
    CHECK(GUEST_DEV_FLAG_FB     == (1u << GUEST_DEV_FB));

    /* GUEST_DEV_COUNT must cover all device types */
    CHECK(GUEST_DEV_COUNT == 5u);
    CHECK(GUEST_DEV_FB < GUEST_DEV_COUNT);

    /* All-devices mask must fit in a uint32_t */
    uint32_t all = GUEST_DEV_FLAG_SERIAL | GUEST_DEV_FLAG_NET |
                   GUEST_DEV_FLAG_BLOCK  | GUEST_DEV_FLAG_USB |
                   GUEST_DEV_FLAG_FB;
    CHECK(all == 0x1Fu);

    PASS("test_device_flags");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Guest state constant tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_guest_states(void)
{
    /* States must be sequential and terminate with DEAD */
    CHECK(GUEST_STATE_CREATING  == 0u);
    CHECK(GUEST_STATE_BINDING   == 1u);
    CHECK(GUEST_STATE_READY     == 2u);
    CHECK(GUEST_STATE_BOOTING   == 3u);
    CHECK(GUEST_STATE_RUNNING   == 4u);
    CHECK(GUEST_STATE_SUSPENDED == 5u);
    CHECK(GUEST_STATE_DEAD      == 6u);

    /* Valid pre-BOOT states: CREATING, BINDING, READY */
    CHECK(GUEST_STATE_READY     <  GUEST_STATE_BOOTING);
    /* Post-BOOT states: BOOTING, RUNNING, SUSPENDED, DEAD */
    CHECK(GUEST_STATE_BOOTING   >  GUEST_STATE_READY);

    PASS("test_guest_states");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MSG_GUEST_CREATE struct tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_guest_create_structs(void)
{
    struct guest_create_req req;
    memset(&req, 0, sizeof(req));

    req.os_type      = VMM_OS_TYPE_LINUX;
    req.device_flags = GUEST_DEV_FLAG_SERIAL | GUEST_DEV_FLAG_NET;
    req.limits.ram_mb          = 512u;
    req.limits.cpu_budget_us   = 5000u;
    req.limits.cpu_period_us   = 10000u;
    req.limits.net_bandwidth_kbps = 100000u;
    req.limits.block_iops      = 0u;
    memcpy(req.label, "test-guest", 10);

    CHECK(req.os_type      == VMM_OS_TYPE_LINUX);
    CHECK(req.limits.ram_mb == 512u);
    CHECK(req.label[0]     == 't');

    struct guest_create_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.ok       = 1u;
    reply.guest_id = 0u;

    CHECK(reply.ok       == 1u);
    CHECK(reply.guest_id == 0u);

    /* guest_id sentinel for error */
    reply.guest_id = 0xFFFFFFFFu;
    CHECK(reply.guest_id == 0xFFFFFFFFu);

    PASS("test_guest_create_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MSG_GUEST_BIND_DEVICE struct tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_guest_bind_device_structs(void)
{
    struct guest_bind_device_req req;
    memset(&req, 0, sizeof(req));
    req.guest_id   = 0u;
    req.dev_type   = GUEST_DEV_NET;
    req.dev_handle = 42u;

    CHECK(req.dev_type   == GUEST_DEV_NET);
    CHECK(req.dev_handle == 42u);

    struct guest_bind_device_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.ok        = 1u;
    reply.cap_token = 0xDEADBEEFu;

    CHECK(reply.ok        == 1u);
    CHECK(reply.cap_token == 0xDEADBEEFu);
    CHECK(reply.cap_token != GUEST_CAP_TOKEN_INVALID);

    PASS("test_guest_bind_device_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MSG_GUEST_SET_MEMORY struct tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_guest_set_memory_structs(void)
{
    struct guest_set_memory_req req;
    memset(&req, 0, sizeof(req));
    req.guest_id     = 0u;
    req.phys_base_lo = 0x40000000u;  /* 1 GiB */
    req.phys_base_hi = 0u;
    req.size_mb      = 512u;
    req.flags        = GUEST_MEM_FLAG_CACHED;

    CHECK(req.phys_base_lo == 0x40000000u);
    CHECK(req.size_mb      == 512u);
    CHECK(req.flags        == GUEST_MEM_FLAG_CACHED);

    /* Flags must not overlap */
    CHECK((GUEST_MEM_FLAG_SHARED & GUEST_MEM_FLAG_CACHED) == 0u);

    struct guest_set_memory_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.ok              = 1u;
    reply.actual_size_mb  = 512u;

    CHECK(reply.ok             == 1u);
    CHECK(reply.actual_size_mb == 512u);

    PASS("test_guest_set_memory_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MSG_GUEST_BOOT, SUSPEND, RESUME struct tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_guest_boot_lifecycle_structs(void)
{
    struct guest_boot_req boot_req;
    memset(&boot_req, 0, sizeof(boot_req));
    boot_req.guest_id        = 0u;
    boot_req.entry_point_lo  = 0x80000000u;
    boot_req.entry_point_hi  = 0u;
    boot_req.boot_arg        = 0xC000u;

    CHECK(boot_req.entry_point_lo == 0x80000000u);
    CHECK(boot_req.boot_arg       == 0xC000u);

    struct guest_boot_reply boot_reply = { .ok = 1u };
    CHECK(boot_reply.ok == 1u);

    struct guest_suspend_req  susp_req  = { .guest_id = 0u };
    struct guest_suspend_reply susp_rep = { .ok = 1u, .state = GUEST_STATE_SUSPENDED };
    CHECK(susp_rep.state == GUEST_STATE_SUSPENDED);

    struct guest_resume_req   res_req   = { .guest_id = 0u };
    struct guest_resume_reply res_rep   = { .ok = 1u, .state = GUEST_STATE_RUNNING };
    CHECK(res_rep.state == GUEST_STATE_RUNNING);

    (void)susp_req; (void)res_req;
    PASS("test_guest_boot_lifecycle_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MSG_GUEST_DESTROY struct tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_guest_destroy_structs(void)
{
    struct guest_destroy_req req;
    memset(&req, 0, sizeof(req));
    req.guest_id = 0u;
    req.reason   = GUEST_DESTROY_NORMAL;
    CHECK(req.reason == GUEST_DESTROY_NORMAL);

    req.reason = GUEST_DESTROY_FORCED;
    CHECK(req.reason == GUEST_DESTROY_FORCED);

    req.reason = GUEST_DESTROY_FAULT;
    CHECK(req.reason == GUEST_DESTROY_FAULT);

    struct guest_destroy_reply reply = { .ok = 1u };
    CHECK(reply.ok == 1u);

    PASS("test_guest_destroy_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * guest_capabilities_t packed layout test
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_guest_capabilities_layout(void)
{
    guest_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));

    caps.serial_token = 0x01u;
    caps.net_token    = 0x02u;
    caps.block_token  = 0x03u;
    caps.usb_token    = 0x04u;
    caps.fb_token     = 0x05u;

    CHECK(caps.serial_token == 0x01u);
    CHECK(caps.net_token    == 0x02u);
    CHECK(caps.block_token  == 0x03u);
    CHECK(caps.usb_token    == 0x04u);
    CHECK(caps.fb_token     == 0x05u);

    /* GUEST_CAP_TOKEN_INVALID must be 0 so memset clears it */
    CHECK(GUEST_CAP_TOKEN_INVALID == 0u);

    /* Packed: 5 tokens + 3 reserved = 8 × uint32_t = 32 bytes */
    CHECK(sizeof(guest_capabilities_t) == 32u);

    PASS("test_guest_capabilities_layout");
}

/* ══════════════════════════════════════════════════════════════════════════
 * guest_error enum completeness
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_guest_error_codes(void)
{
    CHECK(GUEST_OK                     == 0);
    CHECK(GUEST_ERR_BAD_OS_TYPE        == 1);
    CHECK(GUEST_ERR_NO_SLOTS           == 2);
    CHECK(GUEST_ERR_BAD_GUEST_ID       == 3);
    CHECK(GUEST_ERR_BAD_DEV_TYPE       == 4);
    CHECK(GUEST_ERR_DEVICE_UNAVAILABLE == 5);
    CHECK(GUEST_ERR_ALREADY_BOUND      == 6);
    CHECK(GUEST_ERR_MEMORY_CONFLICT    == 7);
    CHECK(GUEST_ERR_MEMORY_SIZE        == 8);
    CHECK(GUEST_ERR_NOT_READY          == 9);
    CHECK(GUEST_ERR_BAD_STATE          == 10);
    CHECK(GUEST_ERR_QUOTA_REJECT       == 11);
    CHECK(GUEST_ERR_PROTOCOL_VIOLATION == 12);
    CHECK(GUEST_ERR_DEAD               == 13);

    PASS("test_guest_error_codes");
}

/* ══════════════════════════════════════════════════════════════════════════
 * VMM Section A — MSG_VM_* struct tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_vm_management_structs(void)
{
    struct vmm_req_create create_req;
    memset(&create_req, 0, sizeof(create_req));
    create_req.os_type      = VMM_OS_TYPE_FREEBSD;
    create_req.ram_mb       = 1024u;
    create_req.device_flags = GUEST_DEV_FLAG_SERIAL | GUEST_DEV_FLAG_BLOCK;

    CHECK(create_req.os_type == VMM_OS_TYPE_FREEBSD);
    CHECK(create_req.ram_mb  == 1024u);

    struct vmm_reply_create create_reply = { .ok = 1u, .vm_id = 0u };
    CHECK(create_reply.vm_id == 0u);
    create_reply.vm_id = 0xFFFFFFFFu;
    CHECK(create_reply.vm_id == 0xFFFFFFFFu);  /* error sentinel */

    struct vmm_reply_status status_reply;
    memset(&status_reply, 0, sizeof(status_reply));
    status_reply.ok          = 1u;
    status_reply.state       = VM_STATE_RUNNING;
    status_reply.ram_used_mb = 256u;
    status_reply.uptime_ticks = 1000u;
    CHECK(status_reply.state == VM_STATE_RUNNING);

    /* VM states */
    CHECK(VM_STATE_CREATING == 0);
    CHECK(VM_STATE_BOOTING  == 1);
    CHECK(VM_STATE_RUNNING  == 2);
    CHECK(VM_STATE_PAUSED   == 3);
    CHECK(VM_STATE_DEAD     == 4);

    /* OS types must be distinct */
    CHECK(VMM_OS_TYPE_LINUX   != VMM_OS_TYPE_FREEBSD);

    /* vm_list_entry_t packed layout */
    CHECK(sizeof(vm_list_entry_t) == 6u * sizeof(uint32_t));

    PASS("test_vm_management_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * VMM Section B — MSG_VMM_* struct tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_vmm_register_structs(void)
{
    struct vmm_register_req req;
    memset(&req, 0, sizeof(req));
    req.os_type    = VMM_OS_TYPE_LINUX;
    req.flags      = VMM_FLAG_SMP;
    req.max_guests = 4u;
    memcpy(req.name, "linux_vmm", 9);

    CHECK(req.os_type    == VMM_OS_TYPE_LINUX);
    CHECK(req.flags      == VMM_FLAG_SMP);
    CHECK(req.max_guests == 4u);

    struct vmm_register_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.ok             = 1u;
    reply.vmm_token      = 0xCAFEu;
    reply.granted_guests = 4u;

    CHECK(reply.vmm_token      == 0xCAFEu);
    CHECK(reply.granted_guests == 4u);

    /* Token 0 means error */
    reply.vmm_token = 0u;
    CHECK(reply.vmm_token == 0u);

    PASS("test_vmm_register_structs");
}

static int test_vmm_alloc_mem_structs(void)
{
    struct vmm_alloc_guest_mem_req req;
    memset(&req, 0, sizeof(req));
    req.vmm_token  = 0xCAFEu;
    req.guest_id   = 0u;
    req.size_mb    = 512u;
    req.align_bits = 21u;  /* 2 MiB huge page alignment */

    CHECK(req.size_mb    == 512u);
    CHECK(req.align_bits == 21u);

    struct vmm_alloc_guest_mem_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.ok             = 1u;
    reply.cap_lo         = 100u;
    reply.cap_count      = 256u;
    reply.actual_size_mb = 512u;

    CHECK(reply.cap_count      == 256u);
    CHECK(reply.actual_size_mb == 512u);

    PASS("test_vmm_alloc_mem_structs");
}

static int test_vmm_vcpu_structs(void)
{
    struct vmm_vcpu_create_req create_req = {
        .vmm_token = 0xCAFEu, .guest_id = 0u
    };
    CHECK(create_req.vmm_token == 0xCAFEu);

    struct vmm_vcpu_create_reply create_reply = {
        .ok = 1u, .vcpu_cap = 7u, .vcpu_index = 0u
    };
    CHECK(create_reply.vcpu_cap   == 7u);
    CHECK(create_reply.vcpu_index == 0u);

    /* Error sentinel */
    create_reply.vcpu_cap = 0xFFFFFFFFu;
    CHECK(create_reply.vcpu_cap == 0xFFFFFFFFu);

    struct vmm_vcpu_destroy_req destroy_req = {
        .vmm_token = 0xCAFEu, .vcpu_cap = 7u
    };
    struct vmm_vcpu_destroy_reply destroy_reply = { .ok = 1u };
    CHECK(destroy_reply.ok == 1u);

    /* VMM_MAX_VCPUS must be at least 1 */
    CHECK(VMM_MAX_VCPUS >= 1u);

    (void)destroy_req;
    PASS("test_vmm_vcpu_structs");
}

static int test_vcpu_regs_layout(void)
{
    vcpu_regs_t regs;
    memset(&regs, 0, sizeof(regs));

    regs.pc    = 0x80000000ULL;
    regs.sp    = 0xFFFF000000000000ULL;
    regs.x[0]  = 0x1234u;
    regs.spsr  = 0x3C5u;   /* EL1h, IRQs masked */
    regs.ttbr0 = 0x40000000ULL;

    CHECK(regs.pc    == 0x80000000ULL);
    CHECK(regs.sp    == 0xFFFF000000000000ULL);
    CHECK(regs.x[0]  == 0x1234u);
    CHECK(regs.x[30] == 0u);   /* last GP reg */

    /* Packed layout: pc + sp + 31 GPRs + spsr + elr + ttbr0 + ttbr1 + vbar = 38 × uint64_t */
    CHECK(sizeof(vcpu_regs_t) == 38u * sizeof(uint64_t) + 4u * sizeof(uint32_t));

    struct vmm_vcpu_set_regs_req set_req = {
        .vmm_token = 0xCAFEu, .vcpu_cap = 7u
    };
    struct vmm_vcpu_set_regs_reply set_reply = { .ok = 1u };

    struct vmm_vcpu_get_regs_req get_req = {
        .vmm_token = 0xCAFEu, .vcpu_cap = 7u
    };
    struct vmm_vcpu_get_regs_reply get_reply = { .ok = 1u };

    CHECK(set_reply.ok == 1u);
    CHECK(get_reply.ok == 1u);
    (void)set_req; (void)get_req;
    PASS("test_vcpu_regs_layout");
}

static int test_vmm_inject_irq_structs(void)
{
    struct vmm_inject_irq_req req;
    memset(&req, 0, sizeof(req));
    req.vmm_token = 0xCAFEu;
    req.guest_id  = 0u;
    req.vcpu_cap  = 7u;
    req.irq_num   = 33u;   /* typical virtio queue IRQ */
    req.level     = 1u;    /* assert */

    CHECK(req.irq_num == 33u);
    CHECK(req.level   == 1u);

    req.level = 0u;
    CHECK(req.level == 0u);  /* deassert */

    struct vmm_inject_irq_reply reply = { .ok = 1u };
    CHECK(reply.ok == 1u);

    PASS("test_vmm_inject_irq_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * vmm_error enum completeness
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_vmm_error_codes(void)
{
    CHECK(VMM_OK                 == 0);
    CHECK(VMM_ERR_NO_SLOTS       == 1);
    CHECK(VMM_ERR_BAD_VM_ID      == 2);
    CHECK(VMM_ERR_BAD_OS_TYPE    == 3);
    CHECK(VMM_ERR_DEAD           == 4);
    CHECK(VMM_ERR_BIND_FAIL      == 5);
    CHECK(VMM_ERR_NOT_REGISTERED == 6);
    CHECK(VMM_ERR_BAD_TOKEN      == 7);
    CHECK(VMM_ERR_NO_MEM         == 8);
    CHECK(VMM_ERR_MAX_VCPUS      == 9);
    CHECK(VMM_ERR_BAD_VCPU_CAP   == 10);
    CHECK(VMM_ERR_BAD_ALIGN      == 11);
    CHECK(VMM_ERR_IRQ_RANGE      == 12);

    PASS("test_vmm_error_codes");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Channel ID tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_channel_ids(void)
{
    /* CH_GUEST_PD and CH_VMM_KERNEL must be defined and distinct */
    CHECK(CH_GUEST_PD   != CH_VMM_KERNEL);
    CHECK(GUEST_PD_CH_CONTROLLER == CH_GUEST_PD);
    CHECK(VMM_KERNEL_CH          == CH_VMM_KERNEL);

    PASS("test_channel_ids");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Protocol ordering invariant test
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_protocol_order_invariants(void)
{
    /*
     * Simulate the required Layer A ordering using state machine:
     * CREATE → BIND_DEVICE(s) → SET_MEMORY → BOOT → SUSPEND → RESUME → DESTROY
     */
    uint32_t state = GUEST_STATE_CREATING;

    /* After CREATE: CREATING */
    CHECK(state == GUEST_STATE_CREATING);

    /* After BIND_DEVICE calls: BINDING */
    state = GUEST_STATE_BINDING;
    CHECK(state == GUEST_STATE_BINDING);
    CHECK(state < GUEST_STATE_BOOTING);   /* cannot BOOT yet */

    /* After SET_MEMORY: READY */
    state = GUEST_STATE_READY;
    CHECK(state == GUEST_STATE_READY);
    CHECK(state < GUEST_STATE_BOOTING);   /* still cannot BOOT until READY */

    /* After BOOT: BOOTING */
    state = GUEST_STATE_BOOTING;
    CHECK(state > GUEST_STATE_READY);
    CHECK(state < GUEST_STATE_RUNNING);

    /* After guest kernel signals: RUNNING */
    state = GUEST_STATE_RUNNING;
    CHECK(state == GUEST_STATE_RUNNING);

    /* SUSPEND */
    state = GUEST_STATE_SUSPENDED;
    CHECK(state != GUEST_STATE_DEAD);

    /* RESUME */
    state = GUEST_STATE_RUNNING;
    CHECK(state == GUEST_STATE_RUNNING);

    /* DESTROY: DEAD */
    state = GUEST_STATE_DEAD;
    CHECK(state == GUEST_STATE_DEAD);

    PASS("test_protocol_order_invariants");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("[guest_contract + vmm_contract] contract unit tests\n");

    int failures = 0;
    failures += test_guest_opcodes();
    failures += test_device_flags();
    failures += test_guest_states();
    failures += test_guest_create_structs();
    failures += test_guest_bind_device_structs();
    failures += test_guest_set_memory_structs();
    failures += test_guest_boot_lifecycle_structs();
    failures += test_guest_destroy_structs();
    failures += test_guest_capabilities_layout();
    failures += test_guest_error_codes();
    failures += test_vm_management_structs();
    failures += test_vmm_register_structs();
    failures += test_vmm_alloc_mem_structs();
    failures += test_vmm_vcpu_structs();
    failures += test_vcpu_regs_layout();
    failures += test_vmm_inject_irq_structs();
    failures += test_vmm_error_codes();
    failures += test_channel_ids();
    failures += test_protocol_order_invariants();

    if (failures == 0)
        printf("\n[guest_contract + vmm_contract] ALL TESTS PASSED\n");
    else
        printf("\n[guest_contract + vmm_contract] %d TEST(S) FAILED\n", failures);

    return failures ? 1 : 0;
}
