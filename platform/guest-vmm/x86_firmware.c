/* Bounded bootstrap interpreter for VM exits, not an instruction emulator. */
#include "x86_firmware.h"
#include <stddef.h>
#include <sel4/arch/vmenter.h>
#include "contracts/guest_execution_caps.h"
#include "contracts/x86_vtx_proof.h"
#include "platform/x86_cpu.h"
#include "platform/x86_config.h"
#ifdef AGENTOS_X86_BOOT_PROFILE
#include <platform/x86_profile.h>
extern const uint8_t _binary_x86_boot_profile_bin_start[], _binary_x86_boot_profile_bin_end[];
#endif
#include "platform/x86_apic.h"
#include "platform/x86_smp.h"
#include "platform/x86_ioapic.h"
#include "platform/x86_virtio.h"
#include "platform/x86_memory.h"
#include "platform/x86_string.h"
#include "platform/x86_event.h"
#include "serial_virt_client.h"
#include <platform/serial_virt_layout.h>
#include <platform/serial_endpoint.h>
#include <platform/vmm_virtio_console.h>
#include <platform/vmm_virtio_blk.h>
#include <platform/blk_layout.h>
#include <contracts/blk_virt_contract.h>
#include <contracts/net_virt_contract.h>
#include <platform/vmm_virtio_net.h>
#include <platform/net_host_layout.h>
#include <platform/guest_teardown.h>
#include <platform/x86_control.h>
#include "contracts/guest_contract.h"
#include "contracts/vmm_contract.h"
#include <libvmm/virtio/gpa.h>
#include "contracts/x86_guest_memory_caps.h"
#include "contracts/guest_queue_caps.h"
#include "x86_guest_objects.h"
#include <platform/x86_memory_rebuild.h>
#include <platform/x86_recreate.h>
#include <platform/guest_ram.h>
#include <platform/serial_rebind.h>
#include <platform/blk_rebind.h>
#include <platform/blk_virt_pump.h>
#include <platform/net_rebind.h>
#include <platform/net_virt_pump.h>
#include <platform/x86_runner_client.h>

/* Matches the independently compiled canonical device adapters. The guest
 * manifest must agree with this image-owned identity; it cannot select it. */
#ifdef AGENTOS_GUEST_SECONDARY
#define X86_GUEST_OWNER 1u
#ifdef AGENTOS_X86_USERSPACE_PROOF
#error "The existing userspace qualification fixture is primary-only"
#endif
#else
#define X86_GUEST_OWNER 0u
#endif
#define X86_GUEST_SERIAL_VA (AOS_SERIAL_SHMEM_VA + X86_GUEST_OWNER * AOS_SERIAL_FRAME_SIZE)

/* The executor's native state and sequence survive guest reconstruction. All
 * device/lifecycle handling stays in this coordinator, between synchronous
 * calls, so a completed reply also establishes that VMEnter has returned. */
typedef struct {
    aos_x86_runner_t runner;
    aos_x86_vmenter_entry_t entry;
    aos_x86_vmenter_return_t returned;
    uint32_t timer_quantum;
} firmware_cpu_t;
static firmware_cpu_t firmware_cpus[2]={
    {.runner={.next_sequence=1}}, {.runner={.next_sequence=1}},
};
static aos_x86_smp_cpu_t firmware_startup[2];
static unsigned selected_cpu;
static firmware_cpu_t *firmware_cpu(void) { return &firmware_cpus[selected_cpu]; }
static bool firmware_init_startup(uint64_t ticks)
{
    for (unsigned cpu=0;cpu<2u;cpu++) {
        firmware_startup[cpu]=(aos_x86_smp_cpu_t){
            .state=cpu ? AOS_X86_CPU_WAIT_SIPI : AOS_X86_CPU_RUNNING};
        if (!aos_x86_apic_init_cpu(&firmware_startup[cpu].apic,ticks,cpu,cpu==0u))
            return false;
    }
    return true;
}
static bool firmware_select(unsigned cpu)
{
    if (cpu>=2u || firmware_cpus[0].runner.active || firmware_cpus[1].runner.active)
        return false;
    selected_cpu=cpu;
    return true;
}
static void firmware_retire(unsigned cpu)
{
    /* Native runner sequences belong to the persistent executor, not the
     * guest generation. Only retire guest state after execution is quiescent. */
    firmware_cpus[cpu].entry=(aos_x86_vmenter_entry_t){0};
    firmware_cpus[cpu].returned=(aos_x86_vmenter_return_t){.result=UINT64_MAX};
    firmware_cpus[cpu].timer_quantum=0;
    firmware_startup[cpu].state=AOS_X86_CPU_WAIT_SIPI;
    firmware_startup[cpu].reset_pending=false;
    firmware_startup[cpu].startup_vector=0;
}
static aos_x86_vmenter_return_t firmware_start(const aos_x86_vmenter_entry_t *entry)
{
    firmware_cpu_t *cpu=firmware_cpu();
    const seL4_CPtr endpoint=selected_cpu ? AOS_X86_AP_RUNNER_ENDPOINT_CAP :
                                          AOS_X86_RUNNER_ENDPOINT_CAP;
    cpu->entry=*entry;
    aos_x86_vmenter_return_t returned={.result=UINT64_MAX};
    (void)aos_x86_runner_call(&cpu->runner,endpoint,&cpu->entry,&returned);
    cpu->returned=returned;
    return returned;
}

#define VCPU (AOS_GUEST_VCPU_CAP_BASE+selected_cpu)
const char vmm_pd_name[] = "guest_vmm_x86";
#define ENTRY 0x4012u
#define CR0 0x6800u
#define CR4 0x6804u
#define CS_BASE 0x6808u
#define CS_RIGHTS 0x4816u
#define RSP 0x681cu
#define CR0_SHADOW 0x6004u
#define CR4_SHADOW 0x6006u
#define EFER 0x2806u
#define PE (1u << 0)
#define PG (1u << 31)
#define PAE (1u << 5)
#define LME (1u << 8)
#define LMA (1u << 10)
#define ENTRY_LONG (1u << 9)
#define PIN_CONTROLS 0x4000u
#define PREEMPTION_COUNTER 0x482eu
#define INTERRUPTIBILITY 0x4824u
#define ACTIVITY 0x4826u
#define IDT_VECTORING 0x4408u
#define ENTRY_EXCEPTION_ERROR_CODE 0x4018u
#ifdef AOS_X86_BOOT_SNAPSHOT_SECONDS
#if AOS_X86_BOOT_SNAPSHOT_SECONDS < 1 || AOS_X86_BOOT_SNAPSHOT_SECONDS > 3600
#error "Intel boot snapshot deadline must be 1..3600 seconds"
#endif
#endif
static seL4_Word timer_exits, injections, eois, timer_shift, tsc_hz, halt_exits;
static seL4_Word snapshot[AOS_X86_FIRMWARE_SNAPSHOT_WORDS];
static seL4_Word halt_chain[AOS_X86_FIRMWARE_CHAIN_WORDS];
static seL4_Word boot_reads[3], last_qualification;
static bool have_wait_snapshot;
static bool serial_wake_received;
static bool serial_attached;
static bool lifecycle_started;
static aos_guest_teardown_t teardown_state;
static seL4_CPtr block_proof_ep;
static uint8_t block_boot_data[AOS_BLK_TRANSFER_SIZE];
extern const uint8_t _binary_x86_firmware_bin_start[], _binary_x86_firmware_bin_end[];
#if defined(AGENTOS_X86_MANAGED_START) && !defined(AGENTOS_X86_USERSPACE_PROOF)
static struct {
    aos_x86_config_t *config, initial_config;
    aos_x86_ioapic_t *ioapic;
    unsigned ioapic_id;
    aos_serial_endpoint_t *serial;
    aos_x86_vmenter_entry_t *entry;
    uint64_t *started;
    unsigned *exits;
    net_virt_rebind_reply_t net;
    blk_virt_rebind_reply_t block;
} reset_context;
static aos_x86_recreate_t reset_transaction;
static bool reset_entry_pending;
static bool control_reset(void);
static void reset_abort(void);
#endif

static uint32_t serial_output(uint8_t *bytes, uint32_t capacity, void *context)
{
    (void)context;
    return aos_vmm_virtio_console_drain_tx(bytes, capacity);
}
static bool serial_input(const uint8_t *bytes, uint32_t length, void *context)
{
    (void)context;
    return aos_vmm_virtio_console_push_rx_bytes(bytes, length);
}
static void service_serial(aos_serial_endpoint_t *endpoint)
{
    if (!serial_attached) return;
    aos_vmm_virtio_console_after_fault();
    const aos_serial_endpoint_ops_t ops = {.output=serial_output, .input=serial_input};
    if (aos_serial_endpoint_step(endpoint, &ops, true))
        seL4_Signal(PD_CNODE_SLOT_SERIAL_VIRT_NOTIFY);
}

bool aos_vmm_serial_detach(void)
{
    if (!serial_attached) return true;
    if (!serial_virt_client_detach(X86_GUEST_OWNER)) return false;
    serial_attached = false;
    return true;
}

/* All native executors have replied before this coordinator handles control.
 * Pausing keeps the coordinator in its control loop. Per-CPU saved state and
 * VMCS remain intact until RESUME; the virtual clock continues advancing. */
static bool control_transition(void)
{
    return !firmware_cpus[0].runner.active && !firmware_cpus[1].runner.active;
}
static bool control_start(void)
{
    lifecycle_started = true;
    return true;
}
static bool control_teardown(void)
{
    if (!control_transition()) return false;
    if (!aos_guest_teardown_step(&teardown_state, AOS_X86_FIRMWARE_RAM)) return false;
    aos_x86_virtio_retire();
    for (unsigned cpu=0;cpu<2u;cpu++) firmware_retire(cpu);
    return true;
}
static void control_wake(seL4_Word badge, void *context)
{
    if (badge & SERIAL_VIRT_VMM_WAKE_BADGE) {
        service_serial(context);
        serial_wake_received = true;
    }
    if ((badge & BLK_VIRT_VMM_WAKE_BADGE) && !teardown_state.block_detached)
        aos_vmm_virtio_blk_resp_ready();
    if ((badge & NET_VIRT_VMM_WAKE_BADGE) && !teardown_state.network_detached)
        aos_vmm_virtio_net_rx_ready();
}
#ifdef AGENTOS_X86_BOOT_KERNEL
extern const uint8_t _binary_x86_boot_kernel_bin_start[], _binary_x86_boot_kernel_bin_end[];
#ifdef AGENTOS_X86_BOOT_INITRD
extern const uint8_t _binary_x86_boot_initrd_bin_start[], _binary_x86_boot_initrd_bin_end[];
#endif
#ifdef AGENTOS_X86_BOOT_CMDLINE
extern const uint8_t _binary_x86_boot_cmdline_bin_start[], _binary_x86_boot_cmdline_bin_end[];
#endif
#endif
_Static_assert(AOS_X86_FIRMWARE_REPORT_WORDS <= seL4_MsgMaxLength,
               "firmware diagnostics must fit in one IPC message");

static _Noreturn void stop(seL4_CPtr endpoint, seL4_Word status, seL4_Word reason,
                 seL4_Word rip, seL4_Word detail)
{
    seL4_SetMR(0, status); seL4_SetMR(1, reason);
    seL4_SetMR(2, rip); seL4_SetMR(3, detail);
    seL4_SetMR(4,timer_exits); seL4_SetMR(5,injections); seL4_SetMR(6,eois);
    seL4_SetMR(7,timer_shift); seL4_SetMR(8,tsc_hz); seL4_SetMR(9,halt_exits);
    for (unsigned i=0; i<AOS_X86_FIRMWARE_SNAPSHOT_WORDS; i++)
        seL4_SetMR(10+i,reason == 0x425544u ? snapshot[i] : 0);
    for (unsigned i=0; i<AOS_X86_FIRMWARE_CHAIN_WORDS; i++)
        seL4_SetMR(10+AOS_X86_FIRMWARE_SNAPSHOT_WORDS+i,
                   reason == 0x425544u ? halt_chain[i] : 0);
    for (unsigned i=0; i<3; i++) seL4_SetMR(116+i,boot_reads[i]);
    seL4_SetMR(119,last_qualification);
    seL4_Send(endpoint, seL4_MessageInfo_new(AOS_X86_VTX_PROOF_LABEL, 0, 0, AOS_X86_FIRMWARE_REPORT_WORDS));
    for (;;) { seL4_Word badge; (void)seL4_Wait(PD_CNODE_SLOT_SELF_EP, &badge); }
}

static void block_wait(void)
{
    seL4_Word badge = 0;
    if (!aos_x86_control_wait_initializing(&badge))
        stop(block_proof_ep, AOS_X86_VTX_PROOF_FAIL, 0x424c4bu, 0, badge);
    if (badge & SERIAL_VIRT_VMM_WAKE_BADGE) serial_wake_received = true;
    if (badge & NET_VIRT_VMM_WAKE_BADGE) aos_vmm_virtio_net_rx_ready();
}

static seL4_Word read_field(seL4_CPtr ep, seL4_Word field)
{
    seL4_X86_VCPU_ReadVMCS_t r = seL4_X86_VCPU_ReadVMCS(VCPU, field);
    if (r.error) stop(ep, AOS_X86_VTX_PROOF_FAIL, field, r.error, 0);
    return r.value;
}

static void write_field(seL4_CPtr ep, seL4_Word field, seL4_Word value)
{
    seL4_X86_VCPU_WriteVMCS_t r = seL4_X86_VCPU_WriteVMCS(VCPU, field, value);
    if (r.error) stop(ep, AOS_X86_VTX_PROOF_FAIL, field, r.error, value);
}

static aos_x86_cpuid_t host_id(uint32_t leaf)
{
    aos_x86_cpuid_t r;
    __asm__ volatile("cpuid" : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                     : "a"(leaf), "c"(0u));
    return r;
}

static uint64_t timestamp(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static aos_x86_vmenter_return_t firmware_run_cpu(seL4_CPtr ep)
{
    firmware_cpu_t *cpu=firmware_cpu();
    /* Arm before the first entry too: an AP trampoline can spin without any
     * emulated I/O while waiting for another guest CPU. */
    if (!cpu->timer_quantum) {
        if (!aos_x86_cpu_clock_supported(tsc_hz))
            stop(ep,AOS_X86_VTX_PROOF_FAIL,0x434c4bu,0,tsc_hz);
        seL4_X86_VCPU_ReadMSR_t misc=seL4_X86_VCPU_ReadMSR(VCPU,0x485u);
        uint64_t tick=UINT64_C(1)<<(misc.value&31u);
        if (misc.error || tick>tsc_hz/1000u || !(misc.value&(1u<<6)))
            stop(ep,AOS_X86_VTX_PROOF_FAIL,0x54494du,0,
                 misc.error ? (uint64_t)misc.error : misc.value);
        timer_shift=misc.value&31u;
        cpu->timer_quantum=(uint32_t)((tsc_hz/1000u+tick-1u)/tick);
        write_field(ep,PIN_CONTROLS,read_field(ep,PIN_CONTROLS)|(1u<<6));
        if (!(read_field(ep,PIN_CONTROLS)&(1u<<6)))
            stop(ep,AOS_X86_VTX_PROOF_FAIL,0x54494du,0,0);
    }
    write_field(ep,PREEMPTION_COUNTER,cpu->timer_quantum);
    return firmware_start(&cpu->entry);
}

static bool firmware_apply_startup(unsigned count)
{
    if (!count || count>2u || !control_transition()) return false;
    const seL4_CPtr pools[]={AOS_X86_VCPU_POOL_CAP,AOS_X86_AP_VCPU_POOL_CAP};
    const seL4_CPtr tcbs[]={AOS_X86_VMM_SELF_TCB_CAP,AOS_X86_AP_RUNNER_TCB_CAP};
    for (unsigned cpu=0;cpu<count;cpu++) {
        aos_x86_smp_cpu_t *startup=&firmware_startup[cpu];
        const seL4_CPtr vcpu=AOS_GUEST_VCPU_CAP_BASE+cpu;
        if (startup->reset_pending) {
            /* ICR processing already retired virtual APIC state. Preserve its
             * requested startup while replacing all native guest state. */
            aos_x86_smp_cpu_t requested=*startup;
            firmware_retire(cpu);
            *startup=requested;
            if (aos_x86_guest_vcpu_rebuild(pools[cpu],vcpu)!=seL4_NoError ||
                aos_x86_guest_vcpu_bind(vcpu,tcbs[cpu])!=seL4_NoError) return false;
            startup->reset_pending=false;
        }
        if (startup->state==AOS_X86_CPU_START_PENDING) {
            seL4_Word failed=0;
            if (aos_x86_firmware_startup_cpu(vcpu,startup->startup_vector,
                    &firmware_cpus[cpu].entry,&failed)!=seL4_NoError) return false;
            startup->state=AOS_X86_CPU_RUNNING;
        }
    }
    return true;
}

#if defined(AGENTOS_X86_MANAGED_START) && !defined(AGENTOS_X86_USERSPACE_PROOF)
/* A failed REBIND reply can be ambiguous: the service may already own the
 * transferred pool. Keep that ownership until a validated DETACH reply. Do
 * not guess its generation or allow another CREATE after an internal error. */
static bool reset_detach(seL4_CPtr ep, uint32_t opcode, uint32_t version,
                          uint32_t request_bytes, uint32_t reply_bytes)
{
    const uint32_t args[4] = {version, X86_GUEST_OWNER,
        opcode == SERIAL_VIRT_OP_DETACH ? SERIAL_VIRT_ROLE_VMM : X86_GUEST_OWNER,
        X86_GUEST_OWNER};
    sel4_msg_t req = {.opcode = opcode, .length = request_bytes}, rep = {0};
    __builtin_memcpy(req.data, args, request_bytes);
    sel4_call(ep, &req, &rep);
    return rep.opcode == SEL4_ERR_OK && rep.length == reply_bytes &&
        msg_u32(&rep, 0u) == 0u && msg_u32(&rep, 4u) == version;
}

_Static_assert(AOS_GUEST_QUEUE_INPUT == AOS_X86_RECREATE_BACKENDS &&
               AOS_GUEST_QUEUE_NET == AOS_X86_RECREATE_NET &&
               AOS_GUEST_QUEUE_BLOCK == AOS_X86_RECREATE_BLK &&
               AOS_GUEST_QUEUE_SERIAL == AOS_X86_RECREATE_SERIAL,
               "reconstruction cleanup must cover every canonical queue");

static void reset_retire(void *context)
{
    (void)context;
    serial_attached = false;
    aos_x86_virtio_retire();
}

static bool reset_backend_detach(void *context, unsigned backend)
{
    (void)context;
    switch (backend) {
    case AOS_X86_RECREATE_NET:
        return reset_detach(PD_CNODE_SLOT_NET_VIRT_EP, NET_VIRT_OP_DETACH,
            NET_VIRT_CONTRACT_VERSION, sizeof(net_virt_attach_req_t),
            sizeof(net_virt_attach_reply_t));
    case AOS_X86_RECREATE_BLK:
        return reset_detach(PD_CNODE_SLOT_BLK_VIRT_EP, BLK_VIRT_OP_DETACH,
            BLK_VIRT_CONTRACT_VERSION, sizeof(blk_virt_attach_req_t),
            sizeof(blk_virt_attach_reply_t));
    case AOS_X86_RECREATE_SERIAL:
        return reset_detach(PD_CNODE_SLOT_SERIAL_VIRT_EP, SERIAL_VIRT_OP_DETACH,
            SERIAL_VIRT_CONTRACT_VERSION, sizeof(serial_virt_attach_req_t),
            sizeof(serial_virt_attach_reply_t));
    default:
        return false;
    }
}

static bool reset_release(void *context, unsigned resource)
{
    (void)context;
    if (resource < AOS_X86_RECREATE_BACKENDS)
        return seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
            AOS_GUEST_QUEUE_POOL_BASE + resource, AOS_GUEST_RAM_CNODE_BITS) == seL4_NoError;
    if (resource == AOS_X86_RECREATE_EXECUTION)
        return seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
            AOS_X86_GUEST_OBJECT_POOL_CAP, AOS_GUEST_RAM_CNODE_BITS) == seL4_NoError;
    return resource == AOS_X86_RECREATE_RAM &&
        aos_vmm_guest_ram_release(AOS_X86_FIRMWARE_RAM);
}

static bool reset_step(void *context, aos_x86_recreate_step_t step, uint32_t generation)
{
    (void)context;
    seL4_Word failed_field = 0u;
    switch (step) {
    case AOS_X86_RECREATE_OBJECTS:
        return aos_x86_guest_objects_rebuild() == seL4_NoError;
    case AOS_X86_RECREATE_MEMORY:
        return aos_x86_guest_memory_rebuild(_binary_x86_firmware_bin_start,
            (size_t)(_binary_x86_firmware_bin_end - _binary_x86_firmware_bin_start),
            AOS_X86_FIRMWARE_RAM);
    case AOS_X86_RECREATE_NATIVE_STATE:
        if (!firmware_select(0u)) return false;
        for (unsigned cpu=0;cpu<2u;cpu++) firmware_retire(cpu);
        *reset_context.config = reset_context.initial_config;
        *reset_context.started = timestamp();
        return firmware_init_startup(*reset_context.started) &&
            aos_x86_ioapic_init(reset_context.ioapic, reset_context.ioapic_id) &&
            aos_x86_virtio_init(reset_context.ioapic, (void *)AOS_X86_FIRMWARE_RAM_VA,
                AOS_X86_FIRMWARE_RAM);
    case AOS_X86_RECREATE_NET_REBIND:
        return aos_net_virt_rebind_with_info(X86_GUEST_OWNER, generation, &reset_context.net);
    case AOS_X86_RECREATE_NET_ADOPT:
        return aos_vmm_virtio_net_adopt(X86_GUEST_OWNER, (void *)AOS_NET_SHMEM_VA,
            &reset_context.net) && aos_vmm_virtio_net_host_ready();
    case AOS_X86_RECREATE_BLK_REBIND:
        return aos_blk_virt_rebind_with_info(X86_GUEST_OWNER, generation, &reset_context.block);
    case AOS_X86_RECREATE_BLK_ADOPT:
        return aos_vmm_virtio_blk_adopt(X86_GUEST_OWNER, (void *)AOS_BLK_SHMEM_VA, &reset_context.block);
    case AOS_X86_RECREATE_SERIAL_REBIND:
        return aos_serial_virt_rebind(X86_GUEST_OWNER, generation);
    case AOS_X86_RECREATE_CONSOLE:
        if (!aos_vmm_virtio_console_recreate()) return false;
        *reset_context.serial = (aos_serial_endpoint_t){
            .channel = aos_serial_channel_at(X86_GUEST_SERIAL_VA)};
        return true;
    case AOS_X86_RECREATE_BIND:
        return aos_x86_guest_objects_bind() == seL4_NoError;
    case AOS_X86_RECREATE_CPU:
        return aos_x86_firmware_reset(reset_context.entry, &failed_field) == seL4_NoError;
    default:
        return false;
    }
}

static void reset_publish(void *context)
{
    (void)context;
    *reset_context.exits = 0u;
    timer_exits = injections = eois = timer_shift = halt_exits = 0u;
    for (unsigned i = 0; i < AOS_X86_FIRMWARE_SNAPSHOT_WORDS; i++) snapshot[i] = 0u;
    for (unsigned i = 0; i < AOS_X86_FIRMWARE_CHAIN_WORDS; i++) halt_chain[i] = 0u;
    for (unsigned i = 0; i < 3u; i++) boot_reads[i] = 0u;
    last_qualification = 0u;
    have_wait_snapshot = serial_wake_received = false;
    teardown_state = (aos_guest_teardown_t){0};
    serial_attached = true;
    reset_entry_pending = true;
}

static const aos_x86_recreate_ops_t reset_ops = {
    .step = reset_step, .publish = reset_publish, .retire = reset_retire,
    .detach = reset_backend_detach, .release = reset_release,
};

static void reset_abort(void)
{
    (void)aos_x86_recreate_cleanup(&reset_transaction, &reset_ops, NULL);
}

static bool control_reset(void)
{
    if (!reset_context.config || !teardown_state.execution_released ||
            !teardown_state.ram_released || !teardown_state.paging_released) return false;
    return aos_x86_recreate_run(&reset_transaction, &reset_ops, NULL);
}
#endif

#ifdef AGENTOS_X86_USERSPACE_PROOF
bool aos_x86_lifecycle_ack;
bool aos_x86_lifecycle_boot_ack;
/* Qualification diagnostics only: no IPC or scheduling until terminal report. */
static uint32_t teardown_proof_stage;
/* Run only after the independent client has destroyed the guest and checked
 * terminal-state rejections. Any later VM entry uses reconstructed execution
 * objects and freshly mapped RAM; retired queues must never be reused. */
static bool recreated_network_proof(uint32_t generation)
{
    const uint32_t stage = 1000u + generation * 100u;
    teardown_proof_stage = stage + 1u;
    net_virt_rebind_reply_t attachment;
    if (!aos_net_virt_rebind_with_info(0u, generation, &attachment)) return false;
    teardown_proof_stage = stage + 2u;
    if (attachment.hw_state != NET_VIRT_HW_NET_PD) return false;
    aos_net_virt_client_t q;
    aos_net_client_bind((uint8_t *)AOS_NET_SHMEM_VA, 0u, &q);
    teardown_proof_stage = stage + 3u;
    if (q.tx_free->head || q.rx_free->head || q.tx_active->head ||
        q.tx_active->tail || q.rx_active->head || q.rx_active->tail ||
        q.tx_free->tail != AOS_NET_CAPACITY || q.rx_free->tail != AOS_NET_CAPACITY)
        return false;
    static const uint8_t arp[] = {
        255,255,255,255,255,255, 0x52,0x54,0,0x12,0x34,0x56, 8,6,
        0,1,8,0,6,4,0,1, 0x52,0x54,0,0x12,0x34,0x56, 10,0,2,15,
        0,0,0,0,0,0, 10,0,2,2
    };
    for (unsigned i = 0; i < sizeof(arp); i++) q.tx_data[i] = arp[i];
    for (unsigned i = 0; i < sizeof(attachment.mac); i++) {
        q.tx_data[6u + i] = attachment.mac[i];
        q.tx_data[22u + i] = attachment.mac[i];
    }
    q.tx_active->buffers[0] = (aos_net_buff_desc_t){.len = sizeof(arp)};
    __atomic_store_n(&q.tx_free->head, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&q.tx_active->tail, 1u, __ATOMIC_RELEASE);
    seL4_Signal(PD_CNODE_SLOT_NET_VIRT_NOTIFY);
    unsigned waits = 0u;
    while ((__atomic_load_n(&q.tx_free->tail, __ATOMIC_ACQUIRE) != AOS_NET_CAPACITY + 1u ||
            __atomic_load_n(&q.rx_active->tail, __ATOMIC_ACQUIRE) == 0u) &&
           waits++ < 100000u) seL4_Yield();
    teardown_proof_stage = stage + 4u;
    if (waits >= 100000u) return false;
    teardown_proof_stage = stage + 5u;
    if (q.tx_active->head != 1u) return false;
    teardown_proof_stage = stage + 6u;
    aos_net_buff_desc_t received = q.rx_active->buffers[0];
    if (!aos_net_buffer_valid(received.io_or_offset, received.len) || received.len < 42u)
        return false;
    const uint8_t *reply = q.rx_data + received.io_or_offset;
    teardown_proof_stage = stage + 7u;
    if (reply[12] != 8u || reply[13] != 6u || reply[20] != 0u || reply[21] != 2u ||
        reply[28] != 10u || reply[29] != 0u || reply[30] != 2u || reply[31] != 2u ||
        reply[38] != 10u || reply[39] != 0u || reply[40] != 2u || reply[41] != 15u)
        return false;
    teardown_proof_stage = stage + 8u;
    for (unsigned i = 0; i < 6u; i++)
        if (reply[i] != attachment.mac[i] || reply[32u + i] != attachment.mac[i]) return false;
    teardown_proof_stage = stage + 9u;
    sel4_msg_t detach = {.opcode = NET_VIRT_OP_DETACH,
        .length = sizeof(net_virt_attach_req_t)}, result = {0};
    const net_virt_attach_req_t args = {NET_VIRT_CONTRACT_VERSION, 0u, 0u};
    __builtin_memcpy(detach.data, &args, sizeof(args));
    sel4_call(PD_CNODE_SLOT_NET_VIRT_EP, &detach, &result);
    if (result.opcode != SEL4_ERR_OK || result.length != sizeof(net_virt_attach_reply_t) ||
        msg_u32(&result, 0u) != NET_VIRT_OK) return false;
    teardown_proof_stage = stage + 10u;
    if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
            AOS_GUEST_QUEUE_POOL_BASE + AOS_GUEST_QUEUE_NET,
            AOS_GUEST_RAM_CNODE_BITS) != seL4_NoError) return false;
    teardown_proof_stage = stage + 11u;
    return seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_QUEUE_TEST_COPY,
        AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
        AOS_GUEST_QUEUE_FRAME_BASE + AOS_GUEST_QUEUE_NET,
        AOS_GUEST_RAM_CNODE_BITS, seL4_AllRights) == seL4_FailedLookup;
}

/* Probe adoption through the same MMIO dispatcher used for guest faults.
 * RAM has been reconstructed, but no VCPU is bound or entered here. */
static bool recreated_network_device_proof(uint32_t generation)
{
    const uint32_t stage = 4000u + generation * 100u;
    teardown_proof_stage = stage + 1u;
    net_virt_rebind_reply_t attachment;
    if (!aos_net_virt_rebind_with_info(0u, generation, &attachment) ||
        attachment.hw_state != NET_VIRT_HW_NET_PD) return false;
    aos_x86_ioapic_t controller;
    teardown_proof_stage = stage + 2u;
    if (!aos_x86_ioapic_init(&controller, 1u) ||
        !aos_x86_virtio_init(&controller, (void *)AOS_X86_FIRMWARE_RAM_VA,
                            AOS_X86_FIRMWARE_RAM)) return false;
    teardown_proof_stage = stage + 3u;
    if (!aos_vmm_virtio_net_adopt(0u, (void *)AOS_NET_SHMEM_VA, &attachment) ||
        !aos_vmm_virtio_net_host_ready() || aos_vmm_virtio_net_guest_io_completed())
        return false;
    const uintptr_t base = AOS_X86_VIRTIO_BASE + 2u * AOS_X86_VIRTIO_STRIDE;
    const uint32_t offsets[] = {0x08u, 0x70u, 0x44u, 0x100u, 0x104u};
    const uint32_t expected[] = {1u, 0u, 0u,
        (uint32_t)attachment.mac[0] | (uint32_t)attachment.mac[1] << 8 |
        (uint32_t)attachment.mac[2] << 16 | (uint32_t)attachment.mac[3] << 24,
        (uint32_t)attachment.mac[4] | (uint32_t)attachment.mac[5] << 8};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        teardown_proof_stage = stage + 10u + i;
        uint32_t value = UINT32_MAX;
        if (!aos_x86_virtio_access(base + offsets[i], 4u, false, &value) ||
            (i == 4u ? value & 0xffffu : value) != expected[i]) return false;
    }
    teardown_proof_stage = stage + 20u;
    if (!aos_vmm_virtio_net_detach()) return false;
    aos_x86_virtio_retire();
    if (virtio_gpa_to_hva(0u, 1u) != NULL) return false;
    teardown_proof_stage = stage + 21u;
    if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
            AOS_GUEST_QUEUE_POOL_BASE + AOS_GUEST_QUEUE_NET,
            AOS_GUEST_RAM_CNODE_BITS) != seL4_NoError) return false;
    teardown_proof_stage = stage + 22u;
    return seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_QUEUE_TEST_COPY,
        AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
        AOS_GUEST_QUEUE_FRAME_BASE + AOS_GUEST_QUEUE_NET,
        AOS_GUEST_RAM_CNODE_BITS, seL4_AllRights) == seL4_FailedLookup;
}

static unsigned rebuilt_block_waits;
static void rebuilt_block_wait(void)
{
    if (++rebuilt_block_waits >= 100000u)
        stop(block_proof_ep, AOS_X86_VTX_PROOF_FAIL, 0x544452u, 0u,
             teardown_proof_stage);
    seL4_Yield();
}

static bool recreated_block_device_proof(uint32_t generation)
{
    const uint32_t stage = 5000u + generation * 100u;
    teardown_proof_stage = stage + 1u;
    blk_virt_rebind_reply_t attachment;
    if (!aos_blk_virt_rebind_with_info(0u, generation, &attachment) ||
        attachment.hw_state != BLK_VIRT_HW_VIRTIO_BLK) return false;
    aos_x86_ioapic_t controller;
    teardown_proof_stage = stage + 2u;
    if (!aos_x86_ioapic_init(&controller, 1u) ||
        !aos_x86_virtio_init(&controller, (void *)AOS_X86_FIRMWARE_RAM_VA,
                            AOS_X86_FIRMWARE_RAM)) return false;
    teardown_proof_stage = stage + 3u;
    if (!aos_vmm_virtio_blk_adopt(0u, (void *)AOS_BLK_SHMEM_VA, &attachment) ||
        aos_vmm_virtio_blk_guest_io_completed()) return false;
    aos_blk_virt_client_t queue;
    aos_blk_client_bind((uint8_t *)AOS_BLK_SHMEM_VA, 0u, &queue);
    uint64_t sectors = queue.info->capacity * (AOS_BLK_TRANSFER_SIZE / 512u);
    const uintptr_t base = AOS_X86_VIRTIO_BASE + AOS_X86_VIRTIO_STRIDE;
    const uint32_t offsets[] = {0x08u, 0x70u, 0x44u, 0x100u, 0x104u, 0x108u};
    const uint32_t expected[] = {2u, 0u, 0u, (uint32_t)sectors,
        (uint32_t)(sectors >> 32), AOS_BLK_GUEST_MAX_SEGMENT_SIZE};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        teardown_proof_stage = stage + 10u + i;
        uint32_t value = UINT32_MAX;
        if (!aos_x86_virtio_access(base + offsets[i], 4u, false, &value) ||
            value != expected[i]) return false;
    }
    teardown_proof_stage = stage + 16u;
    rebuilt_block_waits = 0;
    static uint8_t rebuilt_data[AOS_BLK_TRANSFER_SIZE];
    if (!aos_vmm_virtio_blk_read_boot(0u, 1u, rebuilt_data,
            sizeof(rebuilt_data), rebuilt_block_wait)) return false;
    teardown_proof_stage = stage + 17u;
    for (unsigned i = 0; i < sizeof(rebuilt_data); i++)
        if (rebuilt_data[i] != block_boot_data[i]) return false;
    teardown_proof_stage = stage + 20u;
    if (!aos_vmm_virtio_blk_detach()) return false;
    aos_x86_virtio_retire();
    if (virtio_gpa_to_hva(0u, 1u) != NULL) return false;
    teardown_proof_stage = stage + 21u;
    if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
            AOS_GUEST_QUEUE_POOL_BASE + AOS_GUEST_QUEUE_BLOCK,
            AOS_GUEST_RAM_CNODE_BITS) != seL4_NoError) return false;
    teardown_proof_stage = stage + 22u;
    return seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_QUEUE_TEST_COPY,
        AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
        AOS_GUEST_QUEUE_FRAME_BASE + AOS_GUEST_QUEUE_BLOCK,
        AOS_GUEST_RAM_CNODE_BITS, seL4_AllRights) == seL4_FailedLookup;
}

static bool recreated_console_device_proof(uint32_t generation)
{
    const uint32_t stage = 6000u + generation * 100u;
    teardown_proof_stage = stage + 1u;
    aos_x86_ioapic_t controller;
    if (!aos_x86_ioapic_init(&controller, 1u) ||
        !aos_x86_virtio_init(&controller, (void *)AOS_X86_FIRMWARE_RAM_VA,
                            AOS_X86_FIRMWARE_RAM)) return false;
    teardown_proof_stage = stage + 2u;
    if (!aos_vmm_virtio_console_recreate() || aos_vmm_virtio_console_driver_ready() ||
        aos_vmm_virtio_console_tx_active()) return false;
    uint8_t bytes[16];
    if (aos_vmm_virtio_console_drain_tx(bytes, sizeof(bytes)) != 0u) return false;
    const uint32_t offsets[] = {0x08u, 0x70u, 0x44u};
    const uint32_t expected[] = {3u, 0u, 0u};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        teardown_proof_stage = stage + 10u + i;
        uint32_t value = UINT32_MAX;
        if (!aos_x86_virtio_access(AOS_X86_VIRTIO_BASE + offsets[i], 4u, false, &value) ||
            value != expected[i]) return false;
    }
    teardown_proof_stage = stage + 20u;
    if (!aos_vmm_virtio_console_quiesce()) return false;
    aos_x86_virtio_retire();
    return virtio_gpa_to_hva(0u, 1u) == NULL;
}

static seL4_VCPUContext save_registers(const aos_x86_vmenter_return_t *returned);

/* Scratch-RAM qualification after teardown, before releasing the new pools.
 * Both CPUs retain distinct GPR/x87/SSE values across a native runner switch.
 * This isolates architectural context handling from Linux and its disk. */
static bool reconstructed_cpu_context_proof(seL4_CPtr ep)
{
    const unsigned previous=selected_cpu;
    seL4_VCPUContext expected[2];
    for (unsigned cpu=0;cpu<2u;cpu++) {
        teardown_proof_stage=1100u+cpu;
        if (!firmware_select(cpu)) return false;
        firmware_retire(cpu);
        const seL4_CPtr tcb=cpu ? AOS_X86_AP_RUNNER_TCB_CAP : AOS_X86_VMM_SELF_TCB_CAP;
        seL4_Word failed=0;
        if (aos_x86_guest_vcpu_bind(VCPU,tcb)!=seL4_NoError ||
            aos_x86_firmware_startup_cpu(VCPU,8u+cpu,&firmware_cpu()->entry,&failed)
                !=seL4_NoError) return false;
        /* 16-bit absolute operands address each CPU's own scratch data.
         * fninit; fildl input; movdqu input,xmm0; hlt;
         * movdqu xmm0,output; fistpl output; hlt. No GPR is consumed. */
        uint8_t page=(uint8_t)(0x81u+cpu*0x10u);
        const uint8_t code[]={0xdb,0xe3,0xdb,0x06,0x00,page,
            0xf3,0x0f,0x6f,0x06,0x10,page,0xf4,
            0xf3,0x0f,0x7f,0x06,0x20,page,0xdb,0x1e,0x30,page,0xf4};
        volatile uint8_t *ram=(volatile uint8_t *)AOS_X86_FIRMWARE_RAM_VA;
        for (unsigned i=0;i<sizeof(code);i++) ram[(8u+cpu)*4096u+i]=code[i];
        volatile uint32_t *integer=(volatile uint32_t *)(ram+((unsigned)page<<8));
        *integer=cpu ? 39u : 17u;
        for (unsigned i=0;i<16u;i++) {
            ram[((unsigned)page<<8)+0x10u+i]=(uint8_t)(0x31u+cpu*0x40u+i);
            ram[((unsigned)page<<8)+0x20u+i]=0;
        }
        *(volatile uint32_t *)(ram+((unsigned)page<<8)+0x30u)=0;
        _Static_assert(sizeof(seL4_VCPUContext)==15u*sizeof(seL4_Word),"GPR proof ABI");
        const seL4_Word base=UINT64_C(0x1234000000000000)+(cpu<<16);
        expected[cpu]=(seL4_VCPUContext){.eax=base,.ebx=base+1u,.ecx=base+2u,
            .edx=base+3u,.esi=base+4u,.edi=base+5u,.ebp=base+6u,.r8=base+7u,
            .r9=base+8u,.r10=base+9u,.r11=base+10u,.r12=base+11u,
            .r13=base+12u,.r14=base+13u,.r15=base+14u};
        if (seL4_X86_VCPU_WriteRegisters(VCPU,&expected[cpu])!=seL4_NoError)
            return false;
        write_field(ep,CR4,read_field(ep,CR4)|(1u<<9)); /* OSFXSR for movdqu */
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (unsigned phase=0;phase<2u;phase++) {
        for (unsigned cpu=0;cpu<2u;cpu++) {
            teardown_proof_stage=1200u+phase*100u+cpu*10u;
            if (!firmware_select(cpu)) return false;
            aos_x86_vmenter_return_t returned={0};
            bool halted=false;
            for (unsigned attempt=0;attempt<64u;attempt++) {
                returned=firmware_run_cpu(ep);
                if (returned.result!=SEL4_VMENTER_RESULT_FAULT || returned.badge)
                    return false;
                if (returned.words[SEL4_VMENTER_FAULT_REASON_MR]==12u) {
                    halted=true;
                    break;
                }
                if (returned.words[SEL4_VMENTER_FAULT_REASON_MR]!=52u) return false;
                firmware_cpu()->entry.ip=returned.words[SEL4_VMENTER_CALL_EIP_MR];
            }
            if (!halted || returned.words[SEL4_VMENTER_CALL_EIP_MR]!=(phase ? 23u : 12u) ||
                returned.words[SEL4_VMENTER_FAULT_INSTRUCTION_LEN_MR]!=1u) return false;
            const seL4_VCPUContext observed=save_registers(&returned);
            teardown_proof_stage++;
            if (__builtin_memcmp(&observed,&expected[cpu],sizeof(observed))) return false;
            firmware_cpu()->entry.ip=returned.words[SEL4_VMENTER_CALL_EIP_MR]+1u;
        }
    }
    for (unsigned cpu=0;cpu<2u;cpu++) {
        teardown_proof_stage=1400u+cpu*10u;
        const volatile uint8_t *data=(const volatile uint8_t *)
            (AOS_X86_FIRMWARE_RAM_VA+0x8100u+cpu*0x1000u);
        if (*(const volatile uint32_t *)(data+0x30u)!=(cpu ? 39u : 17u)) return false;
        for (unsigned i=0;i<16u;i++)
            if (data[0x20u+i]!=(uint8_t)(0x31u+cpu*0x40u+i)) return false;
    }
    /* Distinct page-table roots map the same virtual address to four private
     * sentinel pages. Guest MOV CR3 changes each CPU's own address space; the
     * other CPU runs between each observation, with the shared EPT unchanged. */
    volatile uint8_t *ram=(volatile uint8_t *)AOS_X86_FIRMWARE_RAM_VA;
    for (unsigned table=0;table<4u;table++) {
        unsigned base=0x10000u+table*0x3000u;
        volatile uint64_t *pml4=(volatile uint64_t *)(ram+base);
        volatile uint64_t *pdpt=(volatile uint64_t *)(ram+base+0x1000u);
        volatile uint64_t *pd=(volatile uint64_t *)(ram+base+0x2000u);
        for (unsigned i=0;i<512u;i++) pml4[i]=pdpt[i]=pd[i]=0;
        pml4[0]=(base+0x1000u)|3u;
        pdpt[0]=(base+0x2000u)|3u;
        pd[0]=0x83u; /* identity map code, stack and page tables */
        pd[2]=((table+1u)*0x200000u)|0x83u;
        *(volatile uint64_t *)(ram+(table+1u)*0x200000u)=UINT64_C(0xabcd123456780000)+table;
    }
    for (unsigned cpu=0;cpu<2u;cpu++) {
        teardown_proof_stage=1500u+cpu;
        if (!firmware_select(cpu)) return false;
        const uint8_t code[]={0x48,0x8b,0x03,0xf4, /* mov (rbx),rax; hlt */
            0x0f,0x22,0xda,0x48,0x8b,0x03,0xf4, /* mov rdx,cr3; read; hlt */
            0x48,0x8b,0x03,0xf4}; /* retain new translation after peer ran */
        for (unsigned i=0;i<sizeof(code);i++) ram[(8u+cpu)*4096u+i]=code[i];
        seL4_VCPUContext regs={.ebx=0x400000u,.edx=0x10000u+(cpu*2u+1u)*0x3000u};
        if (seL4_X86_VCPU_WriteRegisters(VCPU,&regs)!=seL4_NoError) return false;
        write_field(ep,0x0802u,8u);
        write_field(ep,CS_BASE,0u);
        write_field(ep,CS_RIGHTS,0xa09bu);
        write_field(ep,0x4802u,UINT32_MAX);
        write_field(ep,CR4,read_field(ep,CR4)|PAE);
        write_field(ep,CR4_SHADOW,PAE|(1u<<9));
        write_field(ep,0x6802u,0x10000u+cpu*2u*0x3000u);
        write_field(ep,CR0,PE|PG|0x10u);
        write_field(ep,CR0_SHADOW,PE|PG|0x10u);
        write_field(ep,EFER,LME|LMA);
        write_field(ep,ENTRY,read_field(ep,ENTRY)|ENTRY_LONG|(1u<<15));
        firmware_cpu()->entry=(aos_x86_vmenter_entry_t){(8u+cpu)*4096u,1u<<7,0};
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (unsigned phase=0;phase<3u;phase++) {
        for (unsigned cpu=0;cpu<2u;cpu++) {
            teardown_proof_stage=1600u+phase*100u+cpu*10u;
            if (!firmware_select(cpu)) return false;
            aos_x86_vmenter_return_t returned={0};
            bool halted=false;
            for (unsigned attempt=0;attempt<64u;attempt++) {
                returned=firmware_run_cpu(ep);
                if (returned.result!=SEL4_VMENTER_RESULT_FAULT || returned.badge) return false;
                if (returned.words[SEL4_VMENTER_FAULT_REASON_MR]==12u) { halted=true; break; }
                if (returned.words[SEL4_VMENTER_FAULT_REASON_MR]!=52u) return false;
                firmware_cpu()->entry.ip=returned.words[SEL4_VMENTER_CALL_EIP_MR];
            }
            const unsigned offsets[]={3u,10u,14u};
            unsigned table=cpu*2u+(phase!=0u);
            if (!halted || returned.words[SEL4_VMENTER_CALL_EIP_MR]!=(8u+cpu)*4096u+offsets[phase] ||
                returned.words[SEL4_VMENTER_FAULT_INSTRUCTION_LEN_MR]!=1u) return false;
            teardown_proof_stage++;
            if (returned.words[SEL4_VMENTER_FAULT_EAX]!=UINT64_C(0xabcd123456780000)+table ||
                returned.words[SEL4_VMENTER_FAULT_CR3_MR]!=0x10000u+table*0x3000u) return false;
            firmware_cpu()->entry.ip=returned.words[SEL4_VMENTER_CALL_EIP_MR]+1u;
        }
    }
    for (unsigned cpu=0;cpu<2u;cpu++) firmware_retire(cpu);
    return firmware_select(previous);
}

static bool terminal_teardown_proof(void)
{
    /* Root is already waiting for the terminal report. A failed report on
     * the ordinary service endpoint must not reach that receiver. NBSend
     * drops it when no service receiver is waiting; it cannot block this
     * VMM. The former shared-endpoint wiring would consume and reject it. */
    seL4_SetMR(0, AOS_X86_VTX_PROOF_FAIL);
    seL4_SetMR(1, 0x455052u);
    seL4_SetMR(2, 0u);
    seL4_SetMR(3, 0u);
    seL4_NBSend(PD_CNODE_SLOT_SELF_EP,
        seL4_MessageInfo_new(AOS_X86_VTX_PROOF_LABEL, 0u, 0u, 4u));
    teardown_proof_stage = 1u;
    if (!teardown_state.paging_released || !aos_x86_lifecycle_ack) return false;
    if (virtio_gpa_to_hva(0u, 1u) != NULL) return false;
    for (unsigned slot = 0; slot < AOS_X86_VIRTIO_SLOTS; slot++) {
        teardown_proof_stage = 10u + slot;
        uint64_t address = AOS_X86_VIRTIO_BASE + slot * AOS_X86_VIRTIO_STRIDE;
        uint32_t value = 0xaced1234u;
        if (aos_x86_virtio_contains(address) ||
            aos_x86_virtio_access(address, 4u, false, &value) || value != 0xaced1234u)
            return false;
    }
    aos_x86_virtio_retire();
    teardown_proof_stage = 20u;
    const seL4_CPtr stale[] = {VCPU, AOS_GUEST_RAM_GUEST_VSPACE};
    for (unsigned i = 0; i < 2u; i++) {
        if (seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_QUEUE_TEST_COPY,
                AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
                stale[i], AOS_GUEST_RAM_CNODE_BITS, seL4_AllRights)
                != seL4_FailedLookup) return false;
    }
    /* Successful retyping proves that no original frame/alias survives below
     * the pool. Exercise every pool, including ROM and device queues, twice.
     * These are stopped scratch frames, never a recreated executing guest. */
    for (unsigned pass = 0; pass < 2u; pass++) {
        teardown_proof_stage = 100u + pass * 100u;
        if (!recreated_network_proof(pass * 2u + 1u)) return false;
        teardown_proof_stage = 110u + pass * 100u;
        blk_virt_rebind_reply_t block_attachment;
        if (!aos_blk_virt_rebind_with_info(0u, pass * 2u + 1u, &block_attachment) ||
            block_attachment.hw_state != BLK_VIRT_HW_VIRTIO_BLK) return false;
        aos_blk_virt_client_t rebuilt_block;
        aos_blk_client_bind((uint8_t *)AOS_BLK_SHMEM_VA, 0u, &rebuilt_block);
        if (!rebuilt_block.info->ready || !rebuilt_block.info->capacity ||
            rebuilt_block.req->head || rebuilt_block.req->tail ||
            rebuilt_block.resp->head || rebuilt_block.resp->tail) return false;
        rebuilt_block.req->buffers[0] = (aos_blk_req_t){
            .code = AOS_BLK_REQ_READ, .count = 1u, .id = 0xb10cu + pass};
        __atomic_store_n(&rebuilt_block.req->tail, 1u, __ATOMIC_RELEASE);
        seL4_Signal(PD_CNODE_SLOT_BLK_VIRT_NOTIFY);
        unsigned block_waits = 0;
        while (__atomic_load_n(&rebuilt_block.resp->tail, __ATOMIC_ACQUIRE) != 1u &&
               block_waits++ < 100000u) seL4_Yield();
        if (block_waits >= 100000u || rebuilt_block.req->head != 1u ||
            rebuilt_block.resp->buffers[0].status != AOS_BLK_RESP_OK ||
            rebuilt_block.resp->buffers[0].success_count != 1u ||
            rebuilt_block.resp->buffers[0].id != 0xb10cu + pass) return false;
        __atomic_store_n(&rebuilt_block.resp->head, 1u, __ATOMIC_RELEASE);
        sel4_msg_t block_detach = {.opcode = BLK_VIRT_OP_DETACH,
            .length = sizeof(blk_virt_attach_req_t)}, block_reply = {0};
        const blk_virt_attach_req_t detach_args = {BLK_VIRT_CONTRACT_VERSION, 0u, 0u, 0u};
        __builtin_memcpy(block_detach.data, &detach_args, sizeof(detach_args));
        sel4_call(PD_CNODE_SLOT_BLK_VIRT_EP, &block_detach, &block_reply);
        if (block_reply.opcode != SEL4_ERR_OK ||
            block_reply.length != sizeof(blk_virt_attach_reply_t) ||
            msg_u32(&block_reply, 0u) != BLK_VIRT_OK) return false;
        if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
                AOS_GUEST_QUEUE_POOL_BASE + AOS_GUEST_QUEUE_BLOCK,
                AOS_GUEST_RAM_CNODE_BITS) != seL4_NoError) return false;
        if (seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_QUEUE_TEST_COPY,
                AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
                AOS_GUEST_QUEUE_FRAME_BASE + AOS_GUEST_QUEUE_BLOCK,
                AOS_GUEST_RAM_CNODE_BITS, seL4_AllRights) != seL4_FailedLookup) return false;
        teardown_proof_stage = 120u + pass * 100u;
        if (!aos_serial_virt_rebind(0u, pass + 1u)) return false;
        aos_serial_channel_t rebuilt_serial = aos_serial_channel_at(X86_GUEST_SERIAL_VA);
        static const uint8_t message[] = "x86-recreated-serial\n";
        teardown_proof_stage = 121u + pass * 100u;
        if (aos_serial_queue_write(&rebuilt_serial.from_guest, message,
                sizeof(message) - 1u) != AOS_SERIAL_PUMP_OK) return false;
        seL4_Signal(PD_CNODE_SLOT_SERIAL_VIRT_NOTIFY);
        teardown_proof_stage = 122u + pass * 100u;
        unsigned waits = 0;
        while (__atomic_load_n(&rebuilt_serial.from_guest.queue->head, __ATOMIC_ACQUIRE)
                != sizeof(message) - 1u && waits++ < 100000u) seL4_Yield();
        if (waits >= 100000u) return false;
        teardown_proof_stage = 123u + pass * 100u;
        serial_virt_attach_req_t serial_args = {
            SERIAL_VIRT_CONTRACT_VERSION, 0u, SERIAL_VIRT_ROLE_VMM};
        sel4_msg_t serial_request = {.opcode = SERIAL_VIRT_OP_DETACH,
            .length = sizeof(serial_args)}, serial_reply = {0};
        __builtin_memcpy(serial_request.data, &serial_args, sizeof(serial_args));
        for (unsigned attempt = 0u; ; attempt++) {
            sel4_call(PD_CNODE_SLOT_SERIAL_VIRT_EP, &serial_request, &serial_reply);
            if (serial_reply.opcode != SEL4_ERR_OK ||
                serial_reply.length != sizeof(serial_virt_attach_reply_t) ||
                msg_u32(&serial_reply, 4u) != SERIAL_VIRT_CONTRACT_VERSION) return false;
            uint32_t status = msg_u32(&serial_reply, 0u);
            if (status == SERIAL_VIRT_OK) break;
            /* DETACH closes frontend admission but an already admitted
             * operation may still own the queue. Retry only this contracted
             * transient result; never revoke while access remains live. */
            if (status != SERIAL_VIRT_ERR_BUSY || attempt == 63u) {
                teardown_proof_stage = 3200u + pass * 100u + status;
                return false;
            }
            seL4_Yield();
        }
        teardown_proof_stage = 124u + pass * 100u;
        if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
                AOS_GUEST_QUEUE_POOL_BASE + AOS_GUEST_QUEUE_SERIAL,
                AOS_GUEST_RAM_CNODE_BITS) != seL4_NoError) return false;
        teardown_proof_stage = 125u + pass * 100u;
        if (seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_QUEUE_TEST_COPY,
                AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
                AOS_GUEST_QUEUE_FRAME_BASE + AOS_GUEST_QUEUE_SERIAL,
                AOS_GUEST_RAM_CNODE_BITS, seL4_AllRights) != seL4_FailedLookup) return false;
        for (unsigned group = 0; group < 3u; group++) {
            teardown_proof_stage = 130u + pass * 100u + group;
            unsigned count = group == 0u ? AOS_GUEST_QUEUE_INPUT :
                group == 1u ? AOS_X86_FIRMWARE_RAM >> AOS_GUEST_RAM_FRAME_BITS :
                AOS_X86_GUEST_ROM_FRAMES;
            seL4_CPtr base = group == 0u ? AOS_GUEST_QUEUE_POOL_BASE :
                group == 1u ? AOS_GUEST_RAM_POOL_BASE : AOS_X86_GUEST_ROM_POOL_BASE;
            for (unsigned i = 0; i < count; i++) {
                seL4_CPtr pool = base + i;
                if (seL4_Untyped_Retype(pool, seL4_X86_LargePageObject, 0u,
                        AOS_GUEST_RAM_SELF_CNODE, 0u, 0u,
                        AOS_GUEST_QUEUE_TEST_FRAME, 1u) != seL4_NoError) return false;
                if (seL4_X86_Page_Map(AOS_GUEST_QUEUE_TEST_FRAME,
                        AOS_GUEST_RAM_VMM_VSPACE, AOS_X86_FIRMWARE_RAM_VA,
                        seL4_AllRights, seL4_X86_Default_VMAttributes)
                        != seL4_NoError) return false;
                volatile uint64_t *frame = (volatile uint64_t *)AOS_X86_FIRMWARE_RAM_VA;
                size_t words = ((size_t)1u << AOS_GUEST_RAM_FRAME_BITS) / sizeof(*frame);
                for (size_t n = 0; n < words; n++) {
                    if (frame[n] != 0u) return false;
                    frame[n] = UINT64_C(0xcafe123400000001) ^ n ^ pass;
                }
                if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE, pool,
                        AOS_GUEST_RAM_CNODE_BITS) != seL4_NoError) return false;
                if (seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_QUEUE_TEST_COPY,
                        AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
                        AOS_GUEST_QUEUE_TEST_FRAME, AOS_GUEST_RAM_CNODE_BITS,
                        seL4_AllRights) != seL4_FailedLookup) return false;
            }
        }
        /* Rebuild actual stopped VCPU/EPT objects after complete revocation.
         * This validates retained private allocation/ASID authority, not a
         * recreated executing guest. Binding is checked separately below
         * after restored memory is released; no VM entry occurs. */
        teardown_proof_stage = 140u + pass * 100u;
        if (aos_x86_guest_objects_rebuild() != seL4_NoError) return false;
        if (!aos_x86_guest_memory_rebuild(_binary_x86_firmware_bin_start,
                (size_t)(_binary_x86_firmware_bin_end - _binary_x86_firmware_bin_start),
                AOS_X86_FIRMWARE_RAM)) return false;
        volatile uint64_t *restored_ram = (volatile uint64_t *)AOS_X86_FIRMWARE_RAM_VA;
        for (size_t n = 0; n < AOS_X86_FIRMWARE_RAM / sizeof(*restored_ram); n++) {
            if (restored_ram[n] != 0u) return false;
            restored_ram[n] = UINT64_C(0x1234cafe00000000) ^ n ^ pass;
        }
        const volatile uint8_t *restored_rom = (const volatile uint8_t *)AOS_X86_FIRMWARE_ROM_VA;
        for (size_t n = 0; n < AOS_X86_FIRMWARE_BYTES; n++)
            if (restored_rom[n] != _binary_x86_firmware_bin_start[n]) return false;
        if (!recreated_network_device_proof(pass * 2u + 2u)) return false;
        if (!recreated_block_device_proof(pass * 2u + 2u)) return false;
        if (!recreated_console_device_proof(pass + 1u)) return false;
        /* Execute through the second native runner while both fresh VCPUs
         * exist. The AP starts in real mode at page 8 and exits on HLT. */
        teardown_proof_stage=157u+pass*100u;
        const seL4_CPtr ap_execution=AOS_GUEST_VCPU_CAP_BASE+1u;
        aos_x86_vmenter_entry_t ap_start={0};
        seL4_Word ap_failed=0;
        if (aos_x86_guest_vcpu_bind(ap_execution,AOS_X86_AP_RUNNER_TCB_CAP)!=seL4_NoError ||
            aos_x86_firmware_startup_cpu(ap_execution,8u,&ap_start,&ap_failed)!=seL4_NoError)
            return false;
        const seL4_Word bsp_marker=0x1234abc0u+pass;
        seL4_X86_VCPU_WriteVMCS_t marked=seL4_X86_VCPU_WriteVMCS(VCPU,0x681eu,bsp_marker);
        if (marked.error) return false;
        *(volatile uint8_t *)(AOS_X86_FIRMWARE_RAM_VA+0x8000u)=0xf4u;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        const unsigned prior_cpu=selected_cpu;
        const firmware_cpu_t bsp_context=firmware_cpus[0];
        const uint64_t ap_sequence=firmware_cpus[1].runner.next_sequence;
        if (!firmware_select(1u)) return false;
        aos_x86_vmenter_return_t ap_exit=firmware_start(&ap_start);
        seL4_X86_VCPU_ReadVMCS_t ap_cs=seL4_X86_VCPU_ReadVMCS(VCPU,CS_BASE);
        if (!firmware_select(prior_cpu)) return false;
        if (__builtin_memcmp(&bsp_context,&firmware_cpus[0],sizeof(bsp_context)) ||
            __builtin_memcmp(&ap_exit,&firmware_cpus[1].returned,sizeof(ap_exit)) ||
            firmware_cpus[1].runner.next_sequence!=ap_sequence+1u ||
            ap_cs.error || ap_cs.value!=0x8000u ||
            ap_exit.result!=SEL4_VMENTER_RESULT_FAULT || ap_exit.badge ||
            ap_exit.words[SEL4_VMENTER_FAULT_REASON_MR]!=12u ||
            ap_exit.words[SEL4_VMENTER_CALL_EIP_MR]!=0u ||
            ap_exit.words[SEL4_VMENTER_FAULT_INSTRUCTION_LEN_MR]!=1u) return false;
        seL4_X86_VCPU_ReadVMCS_t unchanged=seL4_X86_VCPU_ReadVMCS(VCPU,0x681eu);
        if (unchanged.error || unchanged.value!=bsp_marker) return false;
        if (aos_x86_guest_vcpu_rebuild(AOS_X86_AP_VCPU_POOL_CAP,ap_execution)!=seL4_NoError)
            return false;
        firmware_retire(1u);
        if (firmware_cpus[1].runner.next_sequence!=ap_sequence+1u ||
            firmware_cpus[1].returned.result!=UINT64_MAX ||
            __builtin_memcmp(&bsp_context,&firmware_cpus[0],sizeof(bsp_context)))
            return false;
        teardown_proof_stage=158u+pass*100u;
        if (!reconstructed_cpu_context_proof(block_proof_ep)) return false;
        teardown_proof_stage = 140u + pass * 100u;
        if (!aos_vmm_guest_ram_release(AOS_X86_FIRMWARE_RAM)) return false;
        const seL4_CPtr retired_frames[] = {AOS_GUEST_RAM_FRAME_BASE,
            AOS_GUEST_RAM_ALIAS_BASE, AOS_X86_GUEST_ROM_FRAME_BASE, AOS_X86_GUEST_ROM_ALIAS_BASE};
        for (unsigned i = 0; i < sizeof(retired_frames) / sizeof(retired_frames[0]); i++) {
            if (seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_QUEUE_TEST_COPY,
                    AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
                    retired_frames[i], AOS_GUEST_RAM_CNODE_BITS, seL4_AllRights)
                    != seL4_FailedLookup) return false;
        }
        const seL4_Word test_rip = 0x123400u + pass;
        seL4_X86_VCPU_WriteVMCS_t wrote =
            seL4_X86_VCPU_WriteVMCS(VCPU, 0x681eu, test_rip);
        seL4_X86_VCPU_ReadVMCS_t read = seL4_X86_VCPU_ReadVMCS(VCPU, 0x681eu);
        if (wrote.error || read.error || read.value != test_rip) return false;
        teardown_proof_stage = 150u + pass * 100u;
        if (aos_x86_guest_objects_bind() != seL4_NoError) return false;
        teardown_proof_stage = 152u + pass * 100u;
        aos_x86_vmenter_entry_t reset_entry = {0};
        seL4_Word failed_field = 0u;
        if (aos_x86_firmware_reset(&reset_entry, &failed_field) != seL4_NoError ||
                failed_field || reset_entry.ip != 0xfff0u ||
                reset_entry.controls != (1u << 7) || reset_entry.interruption_info)
            return false;
        /* Probe the architectural reset state independently of the helper's
         * write/read checks. The stopped VCPU still has no guest memory. */
        teardown_proof_stage = 153u + pass * 100u;
        static const struct { seL4_Word field, value, mask; } reset_fields[] = {
            {0x0802u, 0xf000u, 0xffffu},       /* CS selector */
            {0x6808u, 0xffff0000u, 0xffffffffu}, /* CS base */
            {0x4802u, 0xffffu, 0xffffffffu},  /* CS limit */
            {0x4816u, 0x009bu, 0xffffu},      /* 16-bit code */
            {0x2806u, 0u, 0xffffffffu},       /* EFER */
            {0x6004u, 0x60000010u, 0xffffffffu}, /* CR0 shadow */
            {0x6820u, 2u, 0xffffffffu},       /* RFLAGS */
            {0x4012u, 1u << 15, (1u << 15) | (1u << 9)}, /* EFER load, no IA32e */
        };
        for (unsigned i = 0; i < sizeof(reset_fields) / sizeof(reset_fields[0]); i++) {
            seL4_X86_VCPU_ReadVMCS_t value = seL4_X86_VCPU_ReadVMCS(VCPU,
                reset_fields[i].field);
            if (value.error || (value.value & reset_fields[i].mask) !=
                    reset_fields[i].value) return false;
        }
        /* The reconstructed second VCPU proves startup writes honor the
         * selected capability and cannot alter the bootstrap CPU. */
        teardown_proof_stage = 155u + pass * 100u;
        const seL4_CPtr ap_vcpu=AOS_GUEST_VCPU_CAP_BASE+1u;
        const unsigned vectors[]={0u,8u,255u};
        for (unsigned v=0; v<sizeof(vectors)/sizeof(vectors[0]); v++) {
            aos_x86_vmenter_entry_t ap_entry={0x11u,0x22u,0x33u};
            if (aos_x86_firmware_startup_cpu(ap_vcpu,vectors[v],&ap_entry,&failed_field)!=seL4_NoError ||
                failed_field || ap_entry.ip || ap_entry.controls!=(1u<<7) || ap_entry.interruption_info)
                return false;
            const struct { seL4_Word field,value; } ap_fields[]={
                {0x0802u,(seL4_Word)vectors[v]<<8},
                {0x6808u,(seL4_Word)vectors[v]<<12},
                {0x4802u,0xffffu},{0x4816u,0x009bu},{0x2806u,0u},
                {0x6802u,0u},{0x681cu,0u},{0x4810u,0xffffu},{0x4812u,0xffffu},
            };
            for (unsigned i=0; i<sizeof(ap_fields)/sizeof(ap_fields[0]); i++) {
                seL4_X86_VCPU_ReadVMCS_t value=seL4_X86_VCPU_ReadVMCS(ap_vcpu,ap_fields[i].field);
                if (value.error || value.value!=ap_fields[i].value) return false;
            }
            for (unsigned i=0; i<sizeof(reset_fields)/sizeof(reset_fields[0]); i++) {
                seL4_X86_VCPU_ReadVMCS_t value=seL4_X86_VCPU_ReadVMCS(VCPU,reset_fields[i].field);
                if (value.error || (value.value & reset_fields[i].mask)!=reset_fields[i].value)
                    return false;
            }
        }
        aos_x86_vmenter_entry_t refused_entry={0x11u,0x22u,0x33u};
        failed_field=0x123u;
        if (aos_x86_firmware_startup_cpu(ap_vcpu,256u,&refused_entry,&failed_field)!=seL4_InvalidArgument ||
            refused_entry.ip!=0x11u || refused_entry.controls!=0x22u ||
            refused_entry.interruption_info!=0x33u || failed_field!=0x123u) return false;
        seL4_X86_VCPU_ReadVMCS_t last_base=seL4_X86_VCPU_ReadVMCS(ap_vcpu,0x6808u);
        if (last_base.error || last_base.value!=0xff000u) return false;
        /* Reconstruct only the bootstrap VCPU. Its stale alias disappears,
         * but the sibling VCPU and EPT must remain usable. */
        teardown_proof_stage=156u+pass*100u;
        if (seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE,AOS_GUEST_QUEUE_TEST_COPY,
                AOS_GUEST_RAM_CNODE_BITS,AOS_GUEST_RAM_SELF_CNODE,VCPU,
                AOS_GUEST_RAM_CNODE_BITS,seL4_AllRights)!=seL4_NoError) return false;
        if (aos_x86_guest_vcpu_rebuild(AOS_X86_VCPU_POOL_CAP,VCPU)!=seL4_NoError)
            return false;
        seL4_X86_VCPU_ReadVMCS_t stale_cpu=seL4_X86_VCPU_ReadVMCS(AOS_GUEST_QUEUE_TEST_COPY,0x6808u);
        if (stale_cpu.error==seL4_NoError) return false;
        if (aos_x86_guest_vcpu_bind(VCPU,AOS_X86_VMM_SELF_TCB_CAP)!=seL4_NoError ||
            aos_x86_firmware_reset(&reset_entry,&failed_field)!=seL4_NoError ||
            failed_field || reset_entry.ip!=0xfff0u) return false;
        last_base=seL4_X86_VCPU_ReadVMCS(ap_vcpu,0x6808u);
        if (last_base.error || last_base.value!=0xff000u) return false;
        if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
                AOS_X86_GUEST_OBJECT_POOL_CAP, AOS_GUEST_RAM_CNODE_BITS)
                != seL4_NoError) return false;
        if (seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE,AOS_GUEST_QUEUE_TEST_COPY,
                AOS_GUEST_RAM_CNODE_BITS,AOS_GUEST_RAM_SELF_CNODE,ap_vcpu,
                AOS_GUEST_RAM_CNODE_BITS,seL4_AllRights)!=seL4_FailedLookup) return false;
        if (aos_x86_firmware_startup_cpu(ap_vcpu,8u,&refused_entry,&failed_field)==seL4_NoError ||
            failed_field!=0x0800u || refused_entry.ip!=0x11u || refused_entry.controls!=0x22u ||
            refused_entry.interruption_info!=0x33u) return false;
        for (unsigned i = 0; i < 2u; i++) {
            if (seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_QUEUE_TEST_COPY,
                    AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
                    stale[i], AOS_GUEST_RAM_CNODE_BITS, seL4_AllRights)
                    != seL4_FailedLookup) return false;
        }
        teardown_proof_stage = 151u + pass * 100u;
        if (seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_QUEUE_TEST_COPY,
                AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
                AOS_X86_VMM_SELF_TCB_CAP, AOS_GUEST_RAM_CNODE_BITS,
                seL4_AllRights) != seL4_NoError ||
            seL4_CNode_Delete(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_QUEUE_TEST_COPY,
                AOS_GUEST_RAM_CNODE_BITS) != seL4_NoError) return false;
        teardown_proof_stage = 154u + pass * 100u;
        reset_entry = (aos_x86_vmenter_entry_t){0x11u, 0x22u, 0x33u};
        if (aos_x86_firmware_reset(&reset_entry, &failed_field) == seL4_NoError ||
                failed_field != 0x0800u || reset_entry.ip != 0x11u ||
                reset_entry.controls != 0x22u || reset_entry.interruption_info != 0x33u)
            return false;
    }
    return true;
}
#endif

static seL4_VCPUContext save_registers(const aos_x86_vmenter_return_t *returned)
{
#define REG(name) returned->words[SEL4_VMENTER_FAULT_##name]
    return (seL4_VCPUContext){
        .eax = REG(EAX), .ebx = REG(EBX), .ecx = REG(ECX), .edx = REG(EDX),
        .esi = REG(ESI), .edi = REG(EDI), .ebp = REG(EBP),
        .r8 = REG(R8), .r9 = REG(R9), .r10 = REG(R10), .r11 = REG(R11),
        .r12 = REG(R12), .r13 = REG(R13), .r14 = REG(R14), .r15 = REG(R15),
    };
#undef REG
}

static seL4_Word operand(seL4_CPtr ep, const seL4_VCPUContext *r, unsigned index)
{
    const seL4_Word values[16] = {r->eax, r->ecx, r->edx, r->ebx, 0, r->ebp,
        r->esi, r->edi, r->r8, r->r9, r->r10, r->r11, r->r12, r->r13, r->r14, r->r15};
    return index == 4u ? read_field(ep, RSP) : values[index & 15u];
}

static void assign(seL4_CPtr ep, seL4_VCPUContext *r, unsigned index, uint64_t value)
{
    seL4_Word *values[16] = {&r->eax, &r->ecx, &r->edx, &r->ebx, NULL, &r->ebp,
        &r->esi, &r->edi, &r->r8, &r->r9, &r->r10, &r->r11, &r->r12, &r->r13, &r->r14, &r->r15};
    if (index == 4u) write_field(ep, RSP, value);
    else *values[index] = value;
}

/* Failure-only observation of this guest's private RAM. Never dereference an
 * untranslated guest address or a device GPA, even for diagnostics. */
static void diagnostic_words(const aos_x86_memory_t *m, uint64_t cr3,
                              uint64_t address, unsigned words,
                              seL4_Word *values, seL4_Word *valid)
{
    for (unsigned i=0; i<words; i++) {
        uint64_t value=0;
        if (address > UINT64_MAX-7u) break;
        for (unsigned byte=0; byte<8; byte++) {
            uint64_t pa;
            if (!aos_x86_translate(m,cr3,address+byte,false,false,&pa) ||
                pa >= m->ram_size) {
                return;
            }
            value |= (uint64_t)m->ram[pa] << (byte*8);
        }
        values[i]=value;
        *valid |= UINT64_C(1) << i;
        if (address > UINT64_MAX-8u) break;
        address+=8;
    }
}

static void diagnostic_snapshot(const aos_x86_memory_t *m, uint64_t cr3,
                                 uint64_t rip, uint64_t rsp, seL4_Word *out)
{
    for (unsigned i=0; i<AOS_X86_FIRMWARE_SNAPSHOT_SET_WORDS; i++) out[i]=0;
    out[0]=rip >= 32 ? rip-32 : rip;
    out[1]=rsp;
    diagnostic_words(m,cr3,out[0],AOS_X86_FIRMWARE_CODE_WORDS,out+4,out+2);
    diagnostic_words(m,cr3,out[1],AOS_X86_FIRMWARE_STACK_WORDS,
                     out+4+AOS_X86_FIRMWARE_CODE_WORDS,out+3);
}

static void diagnostic_chain(const aos_x86_memory_t *m, uint64_t cr3, uint64_t rbp)
{
    for (unsigned i=0; i<AOS_X86_FIRMWARE_CHAIN_WORDS; i++) halt_chain[i]=0;
    halt_chain[0]=rbp;
    for (unsigned i=0; i<AOS_X86_FIRMWARE_CHAIN_FRAMES && !(rbp & 7u); i++) {
        seL4_Word pair[2]={0}, valid=0;
        diagnostic_words(m,cr3,rbp,2,pair,&valid);
        if (valid != 3u) break;
        halt_chain[2+2*i]=pair[0]; halt_chain[3+2*i]=pair[1];
        halt_chain[1]++;
        if (pair[0] <= rbp) break;
        rbp=pair[0];
    }
}

_Noreturn void aos_x86_firmware_run(seL4_CPtr ep, aos_x86_vmenter_entry_t entry)
{
#ifdef AGENTOS_X86_USERSPACE_PROOF
    if (serial_virt_client_attach(1u, SERIAL_VIRT_ROLE_VMM) ||
        serial_virt_client_attach(0u, SERIAL_VIRT_ROLE_FRONTEND))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x534552u, 0, 1u);
#endif
    if (!serial_virt_client_attach(X86_GUEST_OWNER, SERIAL_VIRT_ROLE_VMM))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x534552u, 0, 2u);
    serial_attached = true;
    /* Root gives this VMM only its own client page. Check the newly retyped
     * queue state before any producer can publish console bytes. */
    aos_serial_channel_t serial = aos_serial_channel_at(X86_GUEST_SERIAL_VA);
    aos_serial_endpoint_t serial_endpoint = {.channel=serial};
    if (__atomic_load_n(&serial.to_guest.queue->head, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&serial.to_guest.queue->tail, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&serial.from_guest.queue->head, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&serial.from_guest.queue->tail, __ATOMIC_ACQUIRE))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x534552u, 0, 3u);
    /* CPUID is unprivileged. Admit a fixed baseline, never pass host identity
     * or optional hardware facilities through to the guest. */
    if (host_id(0).eax < 1u || host_id(0x80000000u).eax < 0x80000008u ||
        !aos_x86_cpu_supported(host_id(1).edx, host_id(0x80000001u).edx,
                               host_id(0x80000008u).eax)) {
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x435055u, 0, 0);
    }
    aos_x86_config_t config;
    if (!aos_x86_config_init(&config, AOS_X86_FIRMWARE_RAM))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x434647u, 0, 0);
    static aos_x86_acpi_bundle_t acpi;
    unsigned cpu_count=1u;
#ifdef AGENTOS_X86_BOOT_KERNEL
    const aos_x86_boot_blobs_t boot={
        .kernel=_binary_x86_boot_kernel_bin_start,
        .kernel_size=(uint32_t)(_binary_x86_boot_kernel_bin_end-_binary_x86_boot_kernel_bin_start),
#ifdef AGENTOS_X86_BOOT_INITRD
        .initrd=_binary_x86_boot_initrd_bin_start,
        .initrd_size=(uint32_t)(_binary_x86_boot_initrd_bin_end-_binary_x86_boot_initrd_bin_start),
#endif
#ifdef AGENTOS_X86_BOOT_CMDLINE
        .cmdline=_binary_x86_boot_cmdline_bin_start,
        .cmdline_size=(uint32_t)(_binary_x86_boot_cmdline_bin_end-_binary_x86_boot_cmdline_bin_start),
#endif
    };
#ifdef AGENTOS_X86_BOOT_PROFILE
    if (!aos_x86_profile_bind(_binary_x86_boot_profile_bin_start,
            (size_t)(_binary_x86_boot_profile_bin_end-_binary_x86_boot_profile_bin_start),
            &boot, X86_GUEST_OWNER, AOS_X86_FIRMWARE_RAM, AOS_X86_FIRMWARE_RAM_VA))
        stop(ep,AOS_X86_VTX_PROOF_FAIL,0x505246u,0,0);
    cpu_count=((const aos_guest_profile_manifest_t *)_binary_x86_boot_profile_bin_start)->vcpu_count;
#endif
    if (!aos_x86_config_boot(&config,&boot))
        stop(ep,AOS_X86_VTX_PROOF_FAIL,0x424f4fu,0,boot.kernel_size);
#endif
    const aos_x86_acpi_topology_t topology={.lapic_gpa=AOS_X86_APIC_BASE,
        .ioapic_gpa=AOS_X86_IOAPIC_BASE,.ioapic_id=cpu_count>1u ? 15u : 1u,
        .cpu_count=(uint8_t)cpu_count,.cpus={{.uid=0,.apic_id=0},{.uid=1,.apic_id=1}}};
    if (!aos_x86_acpi_bundle_topology(&acpi,&topology) ||
        !aos_x86_config_acpi(&config,&acpi))
        stop(ep,AOS_X86_VTX_PROOF_FAIL,0x41435049u,0,0);
    /* Admit the architectural ratio or an identified KVM board's explicit
     * clock leaf. A missing frequency cannot be replaced by invented time. */
    aos_x86_cpuid_t clock = host_id(0).eax >= 0x15u ? host_id(0x15u) : (aos_x86_cpuid_t){0};
    aos_x86_cpuid_t hypervisor=(host_id(1).ecx & (1u << 31)) ? host_id(0x40000000u) : (aos_x86_cpuid_t){0};
    aos_x86_cpuid_t timing=hypervisor.eax >= 0x40000010u ? host_id(0x40000010u) : (aos_x86_cpuid_t){0};
    uint64_t hz=aos_x86_tsc_frequency(host_id(0x80000007u).edx & (1u << 8),clock,hypervisor,timing);
    tsc_hz=hz;
    uint64_t started = timestamp();
    if (!firmware_init_startup(started))
        stop(ep,AOS_X86_VTX_PROOF_FAIL,0x435055u,0,0);
    aos_x86_ioapic_t ioapic;
    if (!aos_x86_ioapic_init(&ioapic, topology.ioapic_id))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x494f4150u, 0, 1u);
    if (!aos_x86_virtio_init(&ioapic, (void *)AOS_X86_FIRMWARE_RAM_VA,
                            AOS_X86_FIRMWARE_RAM))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x56495254u, 0, AOS_X86_FIRMWARE_RAM);
    if (!aos_vmm_virtio_console_init_at(AOS_X86_VIRTIO_BASE, AOS_X86_VIRTIO_GSI_BASE))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x534552u, 0, 4u);
    if (!aos_vmm_virtio_blk_init_at(X86_GUEST_OWNER, AOS_X86_VIRTIO_BASE + AOS_X86_VIRTIO_STRIDE,
                                   AOS_X86_VIRTIO_GSI_BASE + 1u, (void *)AOS_BLK_SHMEM_VA))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x424c4bu, 0, 1u);
    block_proof_ep = ep;
    if (!aos_vmm_virtio_net_init_at(X86_GUEST_OWNER, AOS_X86_VIRTIO_BASE + 2u * AOS_X86_VIRTIO_STRIDE,
                                   AOS_X86_VIRTIO_GSI_BASE + 2u,
                                   (void *)AGENTOS_NET_SHARED_VA) ||
        !aos_vmm_virtio_net_host_ready())
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x4e4554u, 0, 1u);
    if (!aos_vmm_virtio_blk_read_boot(0u, 1u, block_boot_data,
                                     sizeof(block_boot_data), block_wait))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x424c4bu, 0, 2u);
#if !defined(AGENTOS_X86_LINUX_LOGIN) && !defined(AGENTOS_X86_CC_PCI)
    /* Qualification gates retain their exact fixture check. A distribution
     * root disk has its own partition table and filesystem in this block. */
    static const char expected[] = "agentos-host-block-qualification-v1\n";
    for (unsigned i = 0; i < sizeof(block_boot_data); i++) {
        uint8_t want = i < sizeof(expected) - 1u ? (uint8_t)expected[i] : 0u;
        if (block_boot_data[i] != want)
            stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x424c4bu, 0, 0x100u + i);
    }
#endif
    firmware_cpus[0].entry=entry;
    const aos_x86_memory_t memory = {
        .ram=(const uint8_t *)AOS_X86_FIRMWARE_RAM_VA, .ram_size=AOS_X86_FIRMWARE_RAM,
        .rom=(const uint8_t *)AOS_X86_FIRMWARE_ROM_VA, .rom_base=AOS_X86_FIRMWARE_BASE,
        .rom_size=AOS_X86_FIRMWARE_BYTES,
    };
    unsigned exits = 0;
#if defined(AGENTOS_X86_MANAGED_START) && !defined(AGENTOS_X86_USERSPACE_PROOF)
    reset_context.config = &config;
    reset_context.initial_config = config;
    reset_context.ioapic = &ioapic;
    reset_context.ioapic_id = topology.ioapic_id;
    reset_context.serial = &serial_endpoint;
    reset_context.entry = &firmware_cpus[0].entry;
    reset_context.started = &started;
    reset_context.exits = &exits;
#endif
    uint32_t lifecycle_state = GUEST_STATE_READY;
    lifecycle_started = false;
    const aos_guest_vmm_runtime_t runtime = {
        .os_type = X86_GUEST_OWNER ? VMM_PROFILE_SECONDARY : VMM_PROFILE_PRIMARY,
        .guest_id = X86_GUEST_OWNER,
        .state = &lifecycle_state, .started = &lifecycle_started,
        .start = control_start,
        .suspend = control_transition, .resume = control_transition,
        .teardown = control_teardown,
#if defined(AGENTOS_X86_MANAGED_START) && !defined(AGENTOS_X86_USERSPACE_PROOF)
        .reset = control_reset,
#endif
    };
#ifdef AGENTOS_X86_USERSPACE_PROOF
    seL4_Send(AOS_X86_LIFECYCLE_PROBE_CAP,
        seL4_MessageInfo_new(AOS_X86_LIFECYCLE_READY, 0u, 0u, 0u));
    /* Do not race guest completion against the client's remaining startup
     * Calls. Sending CHECKPOINT while it is calling us would deadlock two
     * synchronous senders. Only boot after it explicitly completes the
     * suspend/resume sequence and commits to receiving the next phase. */
#endif
#ifndef AGENTOS_X86_MANAGED_START
    const sel4_msg_t boot_request = {.opcode = MSG_GUEST_BOOT, .length = 4u};
    sel4_msg_t boot_reply = {0};
    if (!aos_guest_vmm_lifecycle_rpc(&boot_request, &boot_reply, &runtime) ||
        boot_reply.opcode != GUEST_OK || !lifecycle_started)
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x424f54u, 0u, boot_reply.opcode);
#endif
    while (lifecycle_state != GUEST_STATE_RUNNING
#ifdef AGENTOS_X86_USERSPACE_PROOF
           || !aos_x86_lifecycle_boot_ack
#endif
    ) {
        if (aos_x86_control_step(&runtime, control_wake,
                &serial_endpoint) == AOS_X86_CONTROL_ERROR)
            stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x435452u, 0u, lifecycle_state);
        service_serial(&serial_endpoint);
        if (lifecycle_state == GUEST_STATE_RUNNING) seL4_Yield();
    }
    aos_x86_vmenter_return_t returned = firmware_run_cpu(ep);
    bool have_return=true;
    for (;;) {
        enum aos_x86_control_result control;
        do {
#if defined(AGENTOS_X86_MANAGED_START) && !defined(AGENTOS_X86_USERSPACE_PROOF)
            if (reset_transaction.cleanup_pending) reset_abort();
#endif
            control = aos_x86_control_step(&runtime, control_wake, &serial_endpoint);
            if (control == AOS_X86_CONTROL_ERROR)
                stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x435452u, 0u, lifecycle_state);
            if (control == AOS_X86_CONTROL_STOPPED) service_serial(&serial_endpoint);
        } while (control != AOS_X86_CONTROL_RUNNING);
#if defined(AGENTOS_X86_MANAGED_START) && !defined(AGENTOS_X86_USERSPACE_PROOF)
        if (reset_entry_pending) {
            reset_entry_pending = false;
            returned = firmware_run_cpu(ep);
            have_return=true;
            continue;
        }
#endif
        if (!have_return) goto schedule_next;
        aos_x86_apic_t *apic=&firmware_startup[selected_cpu].apic;
        seL4_Word rip = returned.words[SEL4_VMENTER_CALL_EIP_MR];
        if (returned.result == SEL4_VMENTER_RESULT_NOTIF && returned.badge &&
            !(returned.badge & ~(SERIAL_VIRT_VMM_WAKE_BADGE | BLK_VIRT_VMM_WAKE_BADGE |
                                 NET_VIRT_VMM_WAKE_BADGE))) {
            if (returned.badge & SERIAL_VIRT_VMM_WAKE_BADGE) {
                service_serial(&serial_endpoint);
                serial_wake_received = true;
            }
            if (returned.badge & BLK_VIRT_VMM_WAKE_BADGE) aos_vmm_virtio_blk_resp_ready();
            if (returned.badge & NET_VIRT_VMM_WAKE_BADGE) aos_vmm_virtio_net_rx_ready();
            /* Queue completion leaves its IOAPIC line pending. The next
             * bounded VMX timer exit routes it through the common event path. */
            firmware_cpu()->entry=(aos_x86_vmenter_entry_t){
                returned.words[0],returned.words[1],returned.words[2]};
            goto schedule_next;
        }
        if (returned.result != SEL4_VMENTER_RESULT_FAULT)
            stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x4e5446u, rip, returned.badge);
        seL4_Word reason = returned.words[SEL4_VMENTER_FAULT_REASON_MR];
        seL4_Word len = returned.words[SEL4_VMENTER_FAULT_INSTRUCTION_LEN_MR];
        seL4_Word qual = returned.words[SEL4_VMENTER_FAULT_QUALIFICATION_MR];
        seL4_Word fault_gpa = returned.words[SEL4_VMENTER_FAULT_GUEST_PHYSICAL_MR];
        seL4_Word guest_cr3 = returned.words[SEL4_VMENTER_FAULT_CR3_MR];
        seL4_Word guest_flags = returned.words[SEL4_VMENTER_FAULT_RFLAGS_MR];
        seL4_VCPUContext regs = save_registers(&returned);
        for (unsigned i=0; i<3; i++) boot_reads[i]=config.boot_reads[i];
        last_qualification=qual;
#if defined(AGENTOS_X86_LINUX_LOGIN) || defined(AGENTOS_X86_CC_PCI)
        /* Distribution and externally managed guests run until their caller
         * stops them. The small fixture's exit budget is qualification-only. */
        if (exits != UINT32_MAX) exits++;
#ifdef AOS_X86_BOOT_SNAPSHOT_SECONDS
        /* Explicit diagnostic runs stop with failure and retain the bounded
         * code/stack snapshot. This is never a successful login assertion. */
        if (hz && timestamp() - started >= hz * AOS_X86_BOOT_SNAPSHOT_SECONDS) {
            if ((read_field(ep,EFER) & LMA) && (read_field(ep,CR0) & PG)) {
                diagnostic_snapshot(&memory,guest_cr3,rip,read_field(ep,RSP),snapshot);
                diagnostic_chain(&memory,guest_cr3,regs.ebp);
            }
            stop(ep,AOS_X86_VTX_PROOF_FAIL,0x425544u,rip,
                 ((uint64_t)AOS_X86_BOOT_SNAPSHOT_SECONDS << 32) | (uint32_t)reason);
        }
#endif
#else
        if (exits++ == 65536u) {
            /* Observe the returned exit before any emulation or re-entry.
             * The processed-exit budget and its failure status are unchanged. */
            if ((read_field(ep,EFER) & LMA) && (read_field(ep,CR0) & PG)) {
                diagnostic_snapshot(&memory,guest_cr3,rip,read_field(ep,RSP),snapshot);
                if (!have_wait_snapshot) diagnostic_chain(&memory,guest_cr3,regs.ebp);
            }
            stop(ep,AOS_X86_VTX_PROOF_FAIL,0x425544u,rip,
                 (UINT64_C(65536) << 32) | (uint32_t)reason);
        }
#endif
        /* Non-instruction exits do not define an instruction length. */
        if (reason == 52u || reason == 7u) len=0;
        if (len > 15u) {
            stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, len);
        }
        if (read_field(ep, IDT_VECTORING) & (1u << 31))
            stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x564543u, rip, reason);
        firmware_cpu_t *cpu=firmware_cpu();
        uint64_t now = timestamp();
        bool gp=false;
        if (reason == 52u || reason == 7u) {
            /* Timer and interrupt-window exits resume the same instruction. */
            if (reason == 52u) timer_exits++;
        } else if (reason == 12u && len == 1u) {
            if ((read_field(ep,EFER) & LMA) && (read_field(ep,CR0) & PG)) {
                diagnostic_snapshot(&memory,guest_cr3,rip,read_field(ep,RSP),
                    snapshot+AOS_X86_FIRMWARE_SNAPSHOT_SET_WORDS);
                diagnostic_chain(&memory,guest_cr3,regs.ebp);
                have_wait_snapshot=true;
            }
            /* Retain architectural halt until an eligible interrupt arrives.
             * VMX's preemption timer still wakes this VMM from halted state. */
            write_field(ep,ACTIVITY,1u);
            halt_exits++;
        } else if (reason == 10u && len == 2u) {
#ifdef AGENTOS_X86_USERSPACE_PROOF
            if ((uint32_t)regs.eax == AOS_X86_USERSPACE_LEAF) {
                seL4_Word cs=read_field(ep,0x0802u);
                bool passed=serial_wake_received && aos_vmm_virtio_console_driver_ready() &&
                    aos_vmm_virtio_blk_guest_io_completed() &&
                    aos_vmm_virtio_net_guest_io_completed() &&
                    regs.ebx == 1u && regs.ecx == AOS_X86_USERSPACE_INIT &&
                    regs.edx == AOS_X86_USERSPACE_PASS && (cs & 3u) == 3u &&
                    ((read_field(ep,CS_RIGHTS) >> 5) & 3u) == 3u &&
                    (read_field(ep,EFER) & LMA) &&
                    (read_field(ep,CR0) & (PE|PG)) == (PE|PG) &&
                    boot_reads[0] && boot_reads[1];
                if (passed) {
                    AOS_X86_CONTROL_STAGE(10);
                    seL4_Send(AOS_X86_LIFECYCLE_PROBE_CAP,
                        seL4_MessageInfo_new(AOS_X86_LIFECYCLE_CHECKPOINT, 0u, 0u, 0u));
                    AOS_X86_CONTROL_STAGE(11);
                    /* The ring-3 trap is terminal for this fixture. Service
                     * the client's destroy/rejection checks without another
                     * VM entry, even before SUSPEND arrives. */
                    while (!aos_x86_lifecycle_ack) {
                        if (aos_x86_control_step(&runtime, control_wake,
                                &serial_endpoint) == AOS_X86_CONTROL_ERROR)
                            stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x435452u, rip, lifecycle_state);
                        service_serial(&serial_endpoint);
                    }
                }
                if (passed && !terminal_teardown_proof())
                    stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x544452u, rip, teardown_proof_stage);
                stop(ep,passed ? AOS_X86_VTX_LIFECYCLE_PASS : AOS_X86_VTX_PROOF_FAIL,
                     reason,rip,passed ? (cs & 3u) :
                         ((regs.edx == AOS_X86_USERSPACE_PASS ? 0x100u : regs.edx) |
                          ((uint64_t)aos_vmm_virtio_net_diagnostic() << 32)));
            }
#endif
            aos_x86_cpuid_t r;
            if (!aos_x86_cpu_id_topology((uint32_t)regs.eax,(uint32_t)regs.ecx,hz,
                                        acpi.cpu_count,apic->id,&r))
                stop(ep,AOS_X86_VTX_PROOF_FAIL,0x435055u,rip,apic->id);
            regs.eax = r.eax; regs.ebx = r.ebx; regs.ecx = r.ecx; regs.edx = r.edx;
        } else if (reason == 28u && len == 3u && (qual & ~0xf0fu) == 0u &&
                   ((qual & 15u) == 0u || (qual & 15u) == 4u)) {
            /* MOV to CR0/CR4 only; other access types and reserved bits fail. */
            seL4_Word value = operand(ep, &regs, (unsigned)(qual >> 8) & 15u);
            if ((qual & 15u) == 4u) {
                if (value & ~0x6ffu) stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, value);
                write_field(ep, CR4, value);
                write_field(ep, CR4_SHADOW, value);
            } else {
                seL4_Word efer = read_field(ep, EFER);
                seL4_Word entry = read_field(ep, ENTRY);
                if ((value & PG) && (!(value & PE) || !(read_field(ep, CR4) & PAE)))
                    stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, value);
                if ((value & PG) && (efer & LME)) { efer |= LMA; entry |= ENTRY_LONG; }
                else { efer &= ~LMA; entry &= ~ENTRY_LONG; }
                write_field(ep, CR0, value);
                write_field(ep, CR0_SHADOW, value);
                write_field(ep, EFER, efer);
                write_field(ep, ENTRY, entry);
            }
        } else if ((reason == 31u || reason == 32u) && len == 2u &&
                   ((uint32_t)regs.ecx == 0x17u || (uint32_t)regs.ecx == 0x8bu)) {
            uint64_t value = ((uint64_t)(uint32_t)regs.edx << 32) | (uint32_t)regs.eax;
            if (!aos_x86_cpu_identity_msr((uint32_t)regs.ecx, reason == 32u, &value))
                gp=true;
            else if (reason == 31u) { regs.eax=(uint32_t)value; regs.edx=value >> 32; }
        } else if ((reason == 31u || reason == 32u) && len == 2u &&
                   (uint32_t)regs.ecx>=0xc0000081u && (uint32_t)regs.ecx<=0xc0000084u) {
            uint64_t value=((uint64_t)(uint32_t)regs.edx << 32) | (uint32_t)regs.eax;
            if (!aos_x86_cpu_syscall_msr((uint32_t)regs.ecx,reason==32u,value))
                gp=true;
            else if (reason==32u) {
                seL4_X86_VCPU_WriteMSR_t r=seL4_X86_VCPU_WriteMSR(VCPU,(uint32_t)regs.ecx,value);
                if (r.error) stop(ep,AOS_X86_VTX_PROOF_FAIL,reason,rip,r.error);
            } else {
                seL4_X86_VCPU_ReadMSR_t r=seL4_X86_VCPU_ReadMSR(VCPU,(uint32_t)regs.ecx);
                if (r.error) stop(ep,AOS_X86_VTX_PROOF_FAIL,reason,rip,r.error);
                regs.eax=(uint32_t)r.value; regs.edx=r.value >> 32;
            }
        } else if ((reason == 31u || reason == 32u) && len == 2u &&
                   (uint32_t)regs.ecx == 0xc0000080u) {
            seL4_Word efer = read_field(ep, EFER);
            if (reason == 31u) { regs.eax = (uint32_t)efer; regs.edx = efer >> 32; }
            else {
                uint64_t value = ((uint64_t)(uint32_t)regs.edx << 32) | (uint32_t)regs.eax;
                uint64_t next;
                if (!aos_x86_cpu_efer(efer,value,read_field(ep,CR0) & PG,&next))
                    gp=true;
                else write_field(ep, EFER, next);
            }
        } else if ((reason == 31u || reason == 32u) && len == 2u &&
                   (uint32_t)regs.ecx == 0x1bu) {
            uint64_t value = ((uint64_t)(uint32_t)regs.edx << 32) | (uint32_t)regs.eax;
            if (!aos_x86_apic_msr(apic, reason == 32u, &value))
                gp=true;
            else if (reason == 31u) { regs.eax=(uint32_t)value; regs.edx=value >> 32; }
        } else if ((reason == 31u || reason == 32u) && len == 2u) {
            /* Absent MSRs raise #GP(0); do not read host state or fabricate
             * a successful value. Linux's safe probes recover in its IDT. */
            gp=true;
        } else if (reason == 48u &&
                   ((fault_gpa >= AOS_X86_APIC_BASE && fault_gpa < AOS_X86_APIC_BASE+4096) ||
                    aos_x86_virtio_contains(fault_gpa) ||
                    (fault_gpa >= AOS_X86_IOAPIC_BASE && fault_gpa < AOS_X86_IOAPIC_BASE+4096) ||
                    (fault_gpa >= 0xfed40000u && fault_gpa < 0xfed45000u) ||
                    (fault_gpa >= memory.rom_base && fault_gpa-memory.rom_base < memory.rom_size)) &&
                   (qual & 0x180u) == 0x180u &&
                   ((qual & 7u) == 1u || (qual & 7u) == 2u)) {
            if ((read_field(ep, CS_RIGHTS) & 0x6000u) != 0x2000u ||
                !(read_field(ep, EFER) & LMA) || !(read_field(ep, CR0) & PG))
                stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, 0x4d4f4445u);
            if (!(host_id(0x80000007u).edx & (1u << 8)))
                stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x434c4bu, rip, 0x80000007u);
            uint64_t values[16];
            for (unsigned n=0; n<16; n++) values[n]=operand(ep, &regs, n);
            uint8_t code[15];
            aos_x86_mov_t op;
            uint64_t physical;
            if (!aos_x86_fetch(&memory, guest_cr3, rip, code, sizeof(code)) ||
                !aos_x86_decode_mov(code, sizeof(code), rip, values, &op) ||
                op.write != ((qual & 7u) == 2u) || (op.address & (op.width-1u)) ||
                !aos_x86_translate(&memory, guest_cr3, op.address, op.write, false, &physical) ||
                physical != fault_gpa)
                stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, fault_gpa);
            uint32_t value=op.value;
            unsigned eoi_vector = physical == AOS_X86_APIC_BASE+0xb0u && op.write ?
                aos_x86_apic_eoi_vector(apic) : 0u;
            bool handled = physical >= AOS_X86_APIC_BASE && physical < AOS_X86_APIC_BASE+4096 ?
                op.width == 4 && (op.write && physical==AOS_X86_APIC_BASE+0x300u ?
                    aos_x86_smp_icr(firmware_startup,cpu_count,selected_cpu,value,now) :
                    aos_x86_apic_io(apic,(unsigned)(physical-AOS_X86_APIC_BASE),op.write,&value,now)) :
                physical >= AOS_X86_IOAPIC_BASE && physical < AOS_X86_IOAPIC_BASE+4096 ?
                op.width == 4 && aos_x86_ioapic_io(&ioapic, (unsigned)(physical-AOS_X86_IOAPIC_BASE), op.write, &value) :
                aos_x86_virtio_contains(physical) ?
                aos_x86_virtio_access(physical, op.width, op.write, &value) :
                op.write ? aos_x86_rom_store(&memory, physical, op.width) :
                aos_x86_absent_mmio(physical, op.width, false, &value);
            if (!handled)
                stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, physical);
            if (physical == AOS_X86_APIC_BASE+0xb0u && op.write) eois++;
            if (eoi_vector) aos_x86_ioapic_eoi(&ioapic, eoi_vector);
            if (!op.write) assign(ep, &regs, op.reg, aos_x86_mov_result(&op, values[op.reg], value));
            len=op.length;
        } else if (reason == 30u && qual == 0x05110038u && len == 2u) {
            uint8_t code[2];
            if ((read_field(ep, CS_RIGHTS) & 0x6000u) != 0x2000u ||
                !(read_field(ep, EFER) & LMA) || !(read_field(ep, CR0) & PG) ||
                !aos_x86_fetch(&memory, guest_cr3, rip, code, sizeof(code)) ||
                code[0] != 0xf3u || code[1] != 0x6cu)
                stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, qual);
            uint64_t address=regs.edi, count=regs.ecx;
            if (!aos_x86_fw_insb(&memory, (uint8_t *)AOS_X86_FIRMWARE_RAM_VA,
                                 guest_cr3, &config, &address, &count,
                                 (guest_flags & (1u << 10)) != 0))
                stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, regs.edi);
            regs.edi=address; regs.ecx=count;
            if (count) len=0; /* bounded continuation of this REP instruction */
        } else if (reason == 30u && len && !(qual & ~0xffff007fu) &&
                   !(qual & ((1u << 4) | (1u << 5))) && (qual & 7u) != 2u && (qual & 7u) <= 3u) {
            unsigned width = (unsigned)(qual & 7u) + 1u;
            bool write = !(qual & (1u << 3));
            uint16_t port = (uint16_t)(qual >> 16);
            uint32_t value = (uint32_t)regs.eax;
            uint64_t ticks = 0;
            if (hz) {
                uint64_t delta = timestamp() - started;
                ticks = (delta / hz) * 3579545u + ((delta % hz) * 3579545u) / hz;
            }
            /* PM timer reads require a known clock; other ports do not. */
            uint16_t pm_base = ((uint16_t)config.pm[0x41] << 8) | (config.pm[0x40] & 0xc0u);
            /* A terminal interrupt may hide the caller of a firmware delay.
             * Before the first HLT, retain the PM polling site's own stack. */
            if (!halt_exits && pm_base && port==(uint32_t)pm_base+8u &&
                width==4u && !write && (read_field(ep,EFER) & LMA) &&
                (read_field(ep,CR0) & PG)) {
                diagnostic_snapshot(&memory,guest_cr3,rip,read_field(ep,RSP),
                    snapshot+AOS_X86_FIRMWARE_SNAPSHOT_SET_WORDS);
                diagnostic_chain(&memory,guest_cr3,regs.ebp);
                have_wait_snapshot=true;
            }
            if (!hz && pm_base && port == (uint32_t)pm_base + 8u)
                stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x434c4bu, rip, port);
            if (!aos_x86_config_io(&config, port, width, write, &value, ticks)) {
                if (pm_base && port==(uint32_t)pm_base+2u && write)
                    stop(ep,AOS_X86_VTX_PROOF_FAIL,0x504d45u,rip,
                         ((uint64_t)port << 32) | (value & (width==1u ? 0xffu : 0xffffu)));
                if (port == 0x71u)
                    stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x434d4fu, rip,
                         ((uint64_t)config.cmos_index << 32) | value);
                stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, read_field(ep, CS_BASE) + rip, qual);
            }
            if (!write) {
                if (width == 4u) regs.eax = value;
                else {
                    seL4_Word mask = ((seL4_Word)1u << (width * 8u)) - 1u;
                    regs.eax = (regs.eax & ~mask) | (value & mask);
                }
            }
        } else {
            seL4_Word linear = read_field(ep, CS_BASE) + rip;
            stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, linear,
                 reason == 31u || reason == 32u ? (uint32_t)regs.ecx : reason == 48u ? fault_gpa : qual);
        }
        if (config.reset_requested) {
#if defined(AGENTOS_X86_MANAGED_START) && !defined(AGENTOS_X86_USERSPACE_PROOF)
            /* This exit belongs to the retiring context. Never write its
             * registers back or enter either runner while reset is pending. */
            have_return = false;
            enum aos_guest_restart_result result;
            do {
                seL4_Word badge = 0;
                if (!aos_x86_control_poll_initializing(&badge))
                    stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x525354u, rip, 1u);
                if (badge) control_wake(badge, &serial_endpoint);
                result = aos_guest_vmm_restart_step(&runtime);
                if (result == AOS_GUEST_RESTART_WAIT) seL4_Yield();
            } while (result == AOS_GUEST_RESTART_WAIT);
            if (result != AOS_GUEST_RESTART_RUNNING) {
                if (reset_transaction.cleanup_pending) reset_abort();
                stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x525354u, rip, lifecycle_state);
            }
            continue; /* Consume reset_entry_pending at the loop head. */
#else
            stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x525354u, rip, lifecycle_state);
#endif
        }
        service_serial(&serial_endpoint);
        aos_vmm_virtio_blk_after_fault();
        aos_vmm_virtio_net_after_fault();
        /* Self INIT discards this exit's old architectural state. */
        if (firmware_startup[selected_cpu].reset_pending ||
            firmware_startup[selected_cpu].state!=AOS_X86_CPU_RUNNING)
            goto schedule_next;
        seL4_Error err = seL4_X86_VCPU_WriteRegisters(VCPU, &regs);
        if (err) stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, err);
        /* Only VMM-owned emulated sources may assert these inputs. No host
         * device or physical IRQ capability is exposed to the guest. */
        for (unsigned input=0; input<AOS_X86_IOAPIC_INPUTS; input++) {
            aos_x86_ioapic_route_t route;
            if (aos_x86_ioapic_route(&ioapic, input, &route)) {
                bool delivered=false;
                for (unsigned target=0;target<cpu_count;target++)
                    if (firmware_startup[target].state==AOS_X86_CPU_RUNNING &&
                        aos_x86_apic_route(&firmware_startup[target].apic,route.vector,
                                          route.destination,route.logical,route.level))
                        delivered=true;
                if (delivered) aos_x86_ioapic_accept(&ioapic,input);
            }
        }
        unsigned vector=aos_x86_apic_pending(apic,timestamp());
        if (vector == AOS_X86_APIC_INVALID_VECTOR)
            stop(ep,AOS_X86_VTX_PROOF_FAIL,0x495256u,rip,apic->lvt_timer);
        aos_x86_entry_event_t event;
        if (!aos_x86_entry_event(&event,gp,read_field(ep,CR0) & PE,
                                 len,vector,guest_flags,read_field(ep,INTERRUPTIBILITY)))
            stop(ep,AOS_X86_VTX_PROOF_FAIL,0x45564eu,rip,vector);
        seL4_Word controls=(1u << 7) | (event.interrupt_window ? 1u << 2 : 0u);
        if (event.accept_irq) {
            if (!aos_x86_apic_accept(apic,vector))
                stop(ep,AOS_X86_VTX_PROOF_FAIL,0x495251u,rip,vector);
            injections++;
        }
        if (event.interruption_info) write_field(ep,ACTIVITY,0u);
        if (gp) write_field(ep,ENTRY_EXCEPTION_ERROR_CODE,event.error_code);
        cpu->entry=(aos_x86_vmenter_entry_t){rip+event.advance,controls,event.interruption_info};
schedule_next:
        have_return=false;
        if (!firmware_apply_startup(cpu_count))
            stop(ep,AOS_X86_VTX_PROOF_FAIL,0x534d50u,0,selected_cpu);
        unsigned next_cpu=selected_cpu;
        if (!aos_x86_smp_next(firmware_startup,cpu_count,selected_cpu,&next_cpu)) {
            seL4_Yield();
            continue;
        }
        if (!firmware_select(next_cpu))
            stop(ep,AOS_X86_VTX_PROOF_FAIL,0x534d50u,0,next_cpu);
        returned=firmware_run_cpu(ep);
        have_return=true;
    }
}
