/* Bounded bootstrap interpreter for VM exits, not an instruction emulator. */
#include "x86_firmware.h"
#include <stddef.h>
#include <sel4/arch/vmenter.h>
#include "contracts/guest_execution_caps.h"
#include "contracts/x86_vtx_proof.h"
#include "platform/x86_cpu.h"
#include "platform/x86_config.h"
#include "platform/x86_apic.h"
#include "platform/x86_ioapic.h"
#include "platform/x86_virtio.h"
#include "platform/x86_memory.h"
#include "platform/x86_string.h"
#include "platform/x86_event.h"
#include "serial_virt_client.h"
#include <platform/serial_virt_layout.h>
#include <platform/serial_endpoint.h>
#include <platform/vmm_virtio_console.h>

#define VCPU AOS_GUEST_VCPU_CAP_BASE
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
static seL4_Word timer_exits, injections, eois, timer_shift, tsc_hz, halt_exits;
static seL4_Word snapshot[AOS_X86_FIRMWARE_SNAPSHOT_WORDS];
static seL4_Word halt_chain[AOS_X86_FIRMWARE_CHAIN_WORDS];
static seL4_Word boot_reads[3], last_qualification;
static bool have_wait_snapshot;
static bool serial_wake_received;

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
    aos_vmm_virtio_console_after_fault();
    const aos_serial_endpoint_ops_t ops = {.output=serial_output, .input=serial_input};
    if (aos_serial_endpoint_step(endpoint, &ops, true))
        seL4_Signal(PD_CNODE_SLOT_SERIAL_VIRT_NOTIFY);
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
    for (;;) { seL4_Word badge; (void)seL4_Wait(endpoint, &badge); }
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

void aos_x86_firmware_run(seL4_CPtr ep, aos_x86_vmenter_return_t returned)
{
#ifdef AGENTOS_X86_USERSPACE_PROOF
    if (serial_virt_client_attach(1u, SERIAL_VIRT_ROLE_VMM) ||
        serial_virt_client_attach(0u, SERIAL_VIRT_ROLE_FRONTEND))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x534552u, 0, 1u);
#endif
    if (!serial_virt_client_attach(0u, SERIAL_VIRT_ROLE_VMM))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x534552u, 0, 2u);
    /* Root gives this VMM only client zero's page. Check the newly retyped
     * queue state before any producer can publish console bytes. */
    aos_serial_channel_t serial = aos_serial_channel_at(AOS_SERIAL_SHMEM_VA);
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
    if (!aos_x86_acpi_bundle_init(&acpi) || !aos_x86_config_acpi(&config,&acpi))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x41435049u, 0, 0);
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
    if (!aos_x86_config_boot(&config,&boot))
        stop(ep,AOS_X86_VTX_PROOF_FAIL,0x424f4fu,0,boot.kernel_size);
#endif
    /* Admit the architectural ratio or an identified KVM board's explicit
     * clock leaf. A missing frequency cannot be replaced by invented time. */
    aos_x86_cpuid_t clock = host_id(0).eax >= 0x15u ? host_id(0x15u) : (aos_x86_cpuid_t){0};
    aos_x86_cpuid_t hypervisor=(host_id(1).ecx & (1u << 31)) ? host_id(0x40000000u) : (aos_x86_cpuid_t){0};
    aos_x86_cpuid_t timing=hypervisor.eax >= 0x40000010u ? host_id(0x40000010u) : (aos_x86_cpuid_t){0};
    uint64_t hz=aos_x86_tsc_frequency(host_id(0x80000007u).edx & (1u << 8),clock,hypervisor,timing);
    tsc_hz=hz;
    uint64_t started = timestamp();
    aos_x86_apic_t apic;
    aos_x86_apic_init(&apic, started);
    aos_x86_ioapic_t ioapic;
    if (!aos_x86_ioapic_init(&ioapic, 1u))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x494f4150u, 0, 1u);
    if (!aos_x86_virtio_init(&ioapic, (void *)AOS_X86_FIRMWARE_RAM_VA,
                            AOS_X86_FIRMWARE_RAM))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x56495254u, 0, AOS_X86_FIRMWARE_RAM);
    if (!aos_vmm_virtio_console_init_at(AOS_X86_VIRTIO_BASE, AOS_X86_VIRTIO_GSI_BASE))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x534552u, 0, 4u);
    uint32_t timer_quantum=0;
    const aos_x86_memory_t memory = {
        .ram=(const uint8_t *)AOS_X86_FIRMWARE_RAM_VA, .ram_size=AOS_X86_FIRMWARE_RAM,
        .rom=(const uint8_t *)AOS_X86_FIRMWARE_ROM_VA, .rom_base=AOS_X86_FIRMWARE_BASE,
        .rom_size=AOS_X86_FIRMWARE_BYTES,
    };
    unsigned exits = 0;
    for (;;) {
        seL4_Word rip = returned.words[SEL4_VMENTER_CALL_EIP_MR];
        if (returned.result == SEL4_VMENTER_RESULT_NOTIF &&
            returned.badge == SERIAL_VIRT_VMM_WAKE_BADGE) {
            service_serial(&serial_endpoint);
            serial_wake_received = true;
            /* Queue completion leaves its IOAPIC line pending. The next
             * bounded VMX timer exit routes it through the common event path. */
            returned = aos_x86_vm_resume_notification(&returned);
            continue;
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
        /* Non-instruction exits do not define an instruction length. */
        if (reason == 52u || reason == 7u) len=0;
        if (len > 15u) {
            stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, len);
        }
        if (read_field(ep, IDT_VECTORING) & (1u << 31))
            stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x564543u, rip, reason);
        if (!timer_quantum) {
            /* The rate is supplied by the kernel through the VCPU cap, not
             * guessed from a CPU model or read with a privileged instruction. */
            seL4_X86_VCPU_ReadMSR_t misc=seL4_X86_VCPU_ReadMSR(VCPU,0x485u);
            uint64_t tick=UINT64_C(1) << (misc.value & 31u);
            timer_shift=misc.value & 31u;
            if (!aos_x86_cpu_clock_supported(hz))
                stop(ep,AOS_X86_VTX_PROOF_FAIL,0x434c4bu,rip,
                     ((uint64_t)clock.ecx << 32) | ((clock.eax & 0xffffu) << 16) | (clock.ebx & 0xffffu));
            if (misc.error || tick > hz/1000u || !(misc.value & (1u << 6)))
                stop(ep,AOS_X86_VTX_PROOF_FAIL,0x54494du,rip,misc.error ? (uint64_t)misc.error : misc.value);
            timer_quantum=(uint32_t)((hz/1000u+tick-1u)/tick);
            write_field(ep,PIN_CONTROLS,read_field(ep,PIN_CONTROLS) | (1u << 6));
            if (!(read_field(ep,PIN_CONTROLS) & (1u << 6)))
                stop(ep,AOS_X86_VTX_PROOF_FAIL,0x54494du,rip,0);
        }
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
                    regs.ebx == 1u && regs.ecx == AOS_X86_USERSPACE_INIT &&
                    regs.edx == AOS_X86_USERSPACE_PASS && (cs & 3u) == 3u &&
                    ((read_field(ep,CS_RIGHTS) >> 5) & 3u) == 3u &&
                    (read_field(ep,EFER) & LMA) &&
                    (read_field(ep,CR0) & (PE|PG)) == (PE|PG) &&
                    boot_reads[0] && boot_reads[1];
                stop(ep,passed ? AOS_X86_VTX_USERSPACE_PASS : AOS_X86_VTX_PROOF_FAIL,
                     reason,rip,passed ? (cs & 3u) :
                         (regs.edx == AOS_X86_USERSPACE_PASS ? 0x100u : regs.edx));
            }
#endif
            aos_x86_cpuid_t r = aos_x86_cpu_id((uint32_t)regs.eax, (uint32_t)regs.ecx,hz);
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
            if (!aos_x86_apic_msr(reason == 32u, &value))
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
                aos_x86_apic_eoi_vector(&apic) : 0u;
            bool handled = physical >= AOS_X86_APIC_BASE && physical < AOS_X86_APIC_BASE+4096 ?
                op.width == 4 && aos_x86_apic_io(&apic, (unsigned)(physical-AOS_X86_APIC_BASE), op.write, &value, now) :
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
        service_serial(&serial_endpoint);
        seL4_Error err = seL4_X86_VCPU_WriteRegisters(VCPU, &regs);
        if (err) stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, err);
        /* Only VMM-owned emulated sources may assert these inputs. No host
         * device or physical IRQ capability is exposed to the guest. */
        for (unsigned input=0; input<AOS_X86_IOAPIC_INPUTS; input++) {
            aos_x86_ioapic_route_t route;
            if (aos_x86_ioapic_route(&ioapic, input, &route) &&
                aos_x86_apic_route(&apic, route.vector, route.destination,
                                   route.logical, route.level))
                aos_x86_ioapic_accept(&ioapic, input);
        }
        unsigned vector=aos_x86_apic_pending(&apic,timestamp());
        if (vector == AOS_X86_APIC_INVALID_VECTOR)
            stop(ep,AOS_X86_VTX_PROOF_FAIL,0x495256u,rip,apic.lvt_timer);
        aos_x86_entry_event_t event;
        if (!aos_x86_entry_event(&event,gp,read_field(ep,CR0) & PE,
                                 len,vector,guest_flags,read_field(ep,INTERRUPTIBILITY)))
            stop(ep,AOS_X86_VTX_PROOF_FAIL,0x45564eu,rip,vector);
        seL4_Word controls=(1u << 7) | (event.interrupt_window ? 1u << 2 : 0u);
        if (event.accept_irq) {
            if (!aos_x86_apic_accept(&apic,vector))
                stop(ep,AOS_X86_VTX_PROOF_FAIL,0x495251u,rip,vector);
            injections++;
        }
        if (event.interruption_info) write_field(ep,ACTIVITY,0u);
        if (gp) write_field(ep,ENTRY_EXCEPTION_ERROR_CODE,event.error_code);
        write_field(ep,PREEMPTION_COUNTER,timer_quantum);
        seL4_SetMR(SEL4_VMENTER_CALL_EIP_MR, rip + event.advance);
        seL4_SetMR(SEL4_VMENTER_CALL_CONTROL_PPC_MR, controls);
        seL4_SetMR(SEL4_VMENTER_CALL_INTERRUPT_INFO_MR, event.interruption_info);
        returned = aos_x86_vm_enter();
    }
}
