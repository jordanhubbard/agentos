/* Bounded bootstrap interpreter for VM exits, not an instruction emulator. */
#include "x86_firmware.h"
#include <stddef.h>
#include <sel4/arch/vmenter.h>
#include "contracts/guest_execution_caps.h"
#include "contracts/x86_vtx_proof.h"
#include "platform/x86_cpu.h"
#include "platform/x86_config.h"
#include "platform/x86_apic.h"
#include "platform/x86_memory.h"

#define VCPU AOS_GUEST_VCPU_CAP_BASE
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
#define NXE (1u << 11)
#define ENTRY_LONG (1u << 9)

static _Noreturn void stop(seL4_CPtr endpoint, seL4_Word status, seL4_Word reason,
                 seL4_Word rip, seL4_Word detail)
{
    seL4_SetMR(0, status); seL4_SetMR(1, reason);
    seL4_SetMR(2, rip); seL4_SetMR(3, detail);
    seL4_Send(endpoint, seL4_MessageInfo_new(AOS_X86_VTX_PROOF_LABEL, 0, 0, 4));
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

static seL4_VCPUContext save_registers(void)
{
    return (seL4_VCPUContext){
        .eax = seL4_GetMR(SEL4_VMENTER_FAULT_EAX),
        .ebx = seL4_GetMR(SEL4_VMENTER_FAULT_EBX),
        .ecx = seL4_GetMR(SEL4_VMENTER_FAULT_ECX),
        .edx = seL4_GetMR(SEL4_VMENTER_FAULT_EDX),
        .esi = seL4_GetMR(SEL4_VMENTER_FAULT_ESI),
        .edi = seL4_GetMR(SEL4_VMENTER_FAULT_EDI),
        .ebp = seL4_GetMR(SEL4_VMENTER_FAULT_EBP),
        .r8 = seL4_GetMR(SEL4_VMENTER_FAULT_R8),
        .r9 = seL4_GetMR(SEL4_VMENTER_FAULT_R9),
        .r10 = seL4_GetMR(SEL4_VMENTER_FAULT_R10),
        .r11 = seL4_GetMR(SEL4_VMENTER_FAULT_R11),
        .r12 = seL4_GetMR(SEL4_VMENTER_FAULT_R12),
        .r13 = seL4_GetMR(SEL4_VMENTER_FAULT_R13),
        .r14 = seL4_GetMR(SEL4_VMENTER_FAULT_R14),
        .r15 = seL4_GetMR(SEL4_VMENTER_FAULT_R15),
    };
}

static seL4_Word operand(seL4_CPtr ep, const seL4_VCPUContext *r, unsigned index)
{
    const seL4_Word values[16] = {r->eax, r->ecx, r->edx, r->ebx, 0, r->ebp,
        r->esi, r->edi, r->r8, r->r9, r->r10, r->r11, r->r12, r->r13, r->r14, r->r15};
    return index == 4u ? read_field(ep, RSP) : values[index & 15u];
}

static void assign(seL4_CPtr ep, seL4_VCPUContext *r, unsigned index, uint32_t value)
{
    seL4_Word *values[16] = {&r->eax, &r->ecx, &r->edx, &r->ebx, NULL, &r->ebp,
        &r->esi, &r->edi, &r->r8, &r->r9, &r->r10, &r->r11, &r->r12, &r->r13, &r->r14, &r->r15};
    if (index == 4u) write_field(ep, RSP, value);
    else *values[index] = value; /* 32-bit destinations zero-extend in long mode */
}

void aos_x86_firmware_run(seL4_CPtr ep, seL4_Word result)
{
    /* CPUID is unprivileged. Admit a fixed baseline, never pass host identity
     * or optional hardware facilities through to the guest. */
    if (host_id(0).eax < 1u || host_id(0x80000000u).eax < 0x80000008u ||
        !aos_x86_cpu_supported(host_id(1).edx, host_id(0x80000001u).edx,
                               host_id(0x80000008u).eax)) {
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x435055u, 0, 0);
    }
    unsigned cpuid_count = 0;
    aos_x86_config_t config;
    if (!aos_x86_config_init(&config, AOS_X86_FIRMWARE_RAM))
        stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x434647u, 0, 0);
    /* Use the architecturally reported TSC/crystal ratio only. A missing
     * frequency cannot be replaced by invented elapsed time. */
    aos_x86_cpuid_t clock = host_id(0).eax >= 0x15u ? host_id(0x15u) : (aos_x86_cpuid_t){0};
    uint64_t hz = clock.eax ? (uint64_t)clock.ecx * clock.ebx / clock.eax : 0;
    if (hz > 10000000000ull || !(host_id(0x80000007u).edx & (1u << 8))) hz = 0;
    uint64_t started = timestamp();
    aos_x86_apic_t apic;
    aos_x86_apic_init(&apic, started);
    const aos_x86_memory_t memory = {
        .ram=(const uint8_t *)AOS_X86_FIRMWARE_RAM_VA, .ram_size=AOS_X86_FIRMWARE_RAM,
        .rom=(const uint8_t *)AOS_X86_FIRMWARE_ROM_VA, .rom_base=AOS_X86_FIRMWARE_BASE,
        .rom_size=AOS_X86_FIRMWARE_BYTES,
    };
    for (unsigned exits = 0; exits < 65536u; exits++) {
        seL4_Word reason = seL4_GetMR(SEL4_VMENTER_FAULT_REASON_MR);
        seL4_Word rip = seL4_GetMR(SEL4_VMENTER_CALL_EIP_MR);
        seL4_Word len = seL4_GetMR(SEL4_VMENTER_FAULT_INSTRUCTION_LEN_MR);
        seL4_Word qual = seL4_GetMR(SEL4_VMENTER_FAULT_QUALIFICATION_MR);
        seL4_Word fault_gpa = seL4_GetMR(SEL4_VMENTER_FAULT_GUEST_PHYSICAL_MR);
        seL4_Word guest_cr3 = seL4_GetMR(SEL4_VMENTER_FAULT_CR3_MR);
        seL4_VCPUContext regs = save_registers();
        if (result != SEL4_VMENTER_RESULT_FAULT || len > 15u) {
            stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, len);
        }
        uint64_t now = timestamp();
        if (aos_x86_apic_interrupt_due(&apic, now))
            stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x495251u, rip, apic.lvt_timer);
        if (reason == 10u && len == 2u) {
            aos_x86_cpuid_t r = aos_x86_cpu_id((uint32_t)regs.eax, (uint32_t)regs.ecx);
            regs.eax = r.eax; regs.ebx = r.ebx; regs.ecx = r.ecx; regs.edx = r.edx;
            cpuid_count++;
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
                   (uint32_t)regs.ecx == 0xc0000080u) {
            seL4_Word efer = read_field(ep, EFER);
            if (reason == 31u) { regs.eax = (uint32_t)efer; regs.edx = efer >> 32; }
            else {
                uint64_t value = ((uint64_t)(uint32_t)regs.edx << 32) | (uint32_t)regs.eax;
                if ((value & ~(uint64_t)(LME | LMA | NXE)) ||
                    ((read_field(ep, CR0) & PG) && ((value ^ efer) & LME)))
                    stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, value);
                write_field(ep, EFER, (value & ~LMA) | (efer & LMA));
            }
        } else if ((reason == 31u || reason == 32u) && len == 2u &&
                   (uint32_t)regs.ecx == 0x1bu) {
            uint64_t value = ((uint64_t)(uint32_t)regs.edx << 32) | (uint32_t)regs.eax;
            if (!aos_x86_apic_msr(reason == 32u, &value))
                stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, value);
            if (reason == 31u) { regs.eax=(uint32_t)value; regs.edx=value >> 32; }
        } else if (reason == 48u && fault_gpa >= AOS_X86_APIC_BASE &&
                   fault_gpa < AOS_X86_APIC_BASE+4096 && (qual & 0x180u) == 0x180u &&
                   ((qual & 7u) == 1u || (qual & 7u) == 2u)) {
            if ((read_field(ep, CS_RIGHTS) & 0x6000u) != 0x2000u ||
                !(read_field(ep, EFER) & LMA) || !(read_field(ep, CR0) & PG) ||
                !(host_id(0x80000007u).edx & (1u << 8)))
                stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, 0x4d4f4445u);
            uint64_t values[16];
            for (unsigned n=0; n<16; n++) values[n]=operand(ep, &regs, n);
            uint8_t code[15];
            aos_x86_mov_t op;
            uint64_t physical;
            if (!aos_x86_fetch(&memory, guest_cr3, rip, code, sizeof(code)) ||
                !aos_x86_decode_mov32(code, sizeof(code), rip, values, &op) ||
                op.write != ((qual & 7u) == 2u) || (op.address & 3u) ||
                !aos_x86_translate(&memory, guest_cr3, op.address, op.write, false, &physical) ||
                physical != fault_gpa)
                stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, fault_gpa);
            uint32_t value=op.value;
            if (!aos_x86_apic_io(&apic, (unsigned)(physical-AOS_X86_APIC_BASE), op.write, &value, now))
                stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, physical);
            if (!op.write) assign(ep, &regs, op.reg, value);
            len=op.length;
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
            if (!hz && pm_base && port == (uint32_t)pm_base + 8u)
                stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x434c4bu, rip, port);
            if (!aos_x86_config_io(&config, port, width, write, &value, ticks))
                stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, read_field(ep, CS_BASE) + rip, qual);
            if (!write) {
                if (width == 4u) regs.eax = value;
                else {
                    seL4_Word mask = ((seL4_Word)1u << (width * 8u)) - 1u;
                    regs.eax = (regs.eax & ~mask) | (value & mask);
                }
            }
        } else {
            seL4_Word rights = read_field(ep, CS_RIGHTS);
            seL4_Word linear = read_field(ep, CS_BASE) + rip;
            if (reason == 30u && (qual & (1u << 4)) && (qual >> 16) == 0x511u &&
                config.pci_reads && cpuid_count && (rights & 0x6000u) == 0x2000u &&
                (read_field(ep, EFER) & LMA) && (read_field(ep, CR0) & PG)) {
                stop(ep, AOS_X86_VTX_FIRMWARE_CONFIG, reason, linear, qual);
            }
            stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, linear,
                 reason == 31u || reason == 32u ? (uint32_t)regs.ecx : qual);
        }
        seL4_Error err = seL4_X86_VCPU_WriteRegisters(VCPU, &regs);
        if (err) stop(ep, AOS_X86_VTX_PROOF_FAIL, reason, rip, err);
        seL4_SetMR(SEL4_VMENTER_CALL_EIP_MR, rip + len);
        seL4_SetMR(SEL4_VMENTER_CALL_CONTROL_PPC_MR, 1u << 7);
        seL4_SetMR(SEL4_VMENTER_CALL_INTERRUPT_INFO_MR, 0u);
        result = seL4_VMEnter(NULL);
    }
    stop(ep, AOS_X86_VTX_PROOF_FAIL, 0x425544u, 0, 65536u);
}
