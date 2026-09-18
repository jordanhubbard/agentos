/*
 * x86_vtx_proof.c — one-instruction VMX/EPT execution qualification.
 *
 * This VMM is deliberately not a Linux or firmware guest implementation.
 * The root task gives this PD one VCPU bound to its own TCB, an EPT-mapped HLT
 * at guest linear/physical address 0x1000, and four EPT-mapped guest
 * page-table pages. We initialise minimum long-mode VMCS state, enter
 * non-root mode once, and report the expected HLT VM exit to the root task.
 * The optional firmware-mode variant repeats the HLT proof with explicit
 * real-address, unpaged protected and long-mode entry states.
 */

#include <stddef.h>
#include <stdint.h>

#include "sel4_boot.h"
#include "system_desc.h"
#include <sel4/arch/vmenter.h>
#include <platform/x86_vmenter.h>
#include "contracts/guest_execution_caps.h"
#include "contracts/x86_vtx_proof.h"
#ifdef AGENTOS_X86_GUEST_FAULT_PROOF
#include "platform/x86_event.h"
#if defined(AGENTOS_X86_FIRMWARE_RESET) || defined(AGENTOS_X86_FIRMWARE_MODES)
#error "Guest fault proof is a separate private-page composition"
#endif
#endif
#ifdef AGENTOS_X86_FIRMWARE_RESET
#include "x86_firmware.h"
#endif

#ifndef CONFIG_VTX
#error "x86_vtx_proof.c requires a CONFIG_VTX seL4 SDK"
#endif

/* New seL4 names MR2 for its interruption-info payload. The old enum name
 * becomes a deprecated macro there; avoid expanding its Clang-unknown pragma.
 * Microkit 2.1 has the old enum directly, with the same wire slot. */
#ifdef SEL4_VMENTER_CALL_CONTROL_ENTRY_MR
#define AOS_VMENTER_INTERRUPT_INFO_MR SEL4_VMENTER_CALL_INTERRUPT_INFO_MR
#else
#define AOS_VMENTER_INTERRUPT_INFO_MR SEL4_VMENTER_CALL_CONTROL_ENTRY_MR
#endif

#define VTX_GUEST_CODE_SELECTOR       0x08u
#define VTX_GUEST_DATA_SELECTOR       0x10u
#define VTX_GUEST_TR_SELECTOR         0x18u
#define VTX_GUEST_GDT_LIMIT           0x1fu
#define VTX_GUEST_FLAT_LIMIT  0xffffffffu

/* Intel VMCS field encodings used by this bounded proof. */
#define VMX_GUEST_ES_SELECTOR          0x00000800u
#define VMX_GUEST_CS_SELECTOR          0x00000802u
#define VMX_GUEST_SS_SELECTOR          0x00000804u
#define VMX_GUEST_DS_SELECTOR          0x00000806u
#define VMX_GUEST_FS_SELECTOR          0x00000808u
#define VMX_GUEST_GS_SELECTOR          0x0000080au
#define VMX_GUEST_LDTR_SELECTOR        0x0000080cu
#define VMX_GUEST_TR_SELECTOR          0x0000080eu

#define VMX_GUEST_DEBUGCTRL            0x00002802u
#define VMX_GUEST_EFER                 0x00002806u

#define VMX_GUEST_ES_LIMIT             0x00004800u
#define VMX_GUEST_CS_LIMIT             0x00004802u
#define VMX_GUEST_SS_LIMIT             0x00004804u
#define VMX_GUEST_DS_LIMIT             0x00004806u
#define VMX_GUEST_FS_LIMIT             0x00004808u
#define VMX_GUEST_GS_LIMIT             0x0000480au
#define VMX_GUEST_LDTR_LIMIT           0x0000480cu
#define VMX_GUEST_TR_LIMIT             0x0000480eu
#define VMX_GUEST_GDTR_LIMIT           0x00004810u
#define VMX_GUEST_IDTR_LIMIT           0x00004812u

#define VMX_GUEST_ES_ACCESS_RIGHTS     0x00004814u
#define VMX_GUEST_CS_ACCESS_RIGHTS     0x00004816u
#define VMX_GUEST_SS_ACCESS_RIGHTS     0x00004818u
#define VMX_GUEST_DS_ACCESS_RIGHTS     0x0000481au
#define VMX_GUEST_FS_ACCESS_RIGHTS     0x0000481cu
#define VMX_GUEST_GS_ACCESS_RIGHTS     0x0000481eu
#define VMX_GUEST_LDTR_ACCESS_RIGHTS   0x00004820u
#define VMX_GUEST_TR_ACCESS_RIGHTS     0x00004822u
#define VMX_GUEST_INTERRUPTABILITY     0x00004824u
#define VMX_GUEST_ACTIVITY             0x00004826u
#define VMX_GUEST_SMBASE               0x00004828u
#define VMX_GUEST_SYSENTER_CS          0x0000482au

#define VMX_GUEST_CR0                  0x00006800u
#define VMX_GUEST_CR3                  0x00006802u
#define VMX_GUEST_CR4                  0x00006804u
#define VMX_GUEST_ES_BASE              0x00006806u
#define VMX_GUEST_CS_BASE              0x00006808u
#define VMX_GUEST_SS_BASE              0x0000680au
#define VMX_GUEST_DS_BASE              0x0000680cu
#define VMX_GUEST_FS_BASE              0x0000680eu
#define VMX_GUEST_GS_BASE              0x00006810u
#define VMX_GUEST_LDTR_BASE            0x00006812u
#define VMX_GUEST_TR_BASE              0x00006814u
#define VMX_GUEST_GDTR_BASE            0x00006816u
#define VMX_GUEST_IDTR_BASE            0x00006818u
#define VMX_GUEST_DR7                  0x0000681au
#define VMX_GUEST_RSP                  0x0000681cu
#define VMX_GUEST_RFLAGS               0x00006820u
#define VMX_GUEST_PENDING_DEBUG        0x00006822u
#define VMX_GUEST_SYSENTER_ESP         0x00006824u
#define VMX_GUEST_SYSENTER_EIP         0x00006826u

#define VMX_CONTROL_CR0_MASK           0x00006000u
#define VMX_CONTROL_CR4_MASK           0x00006002u
#define VMX_CONTROL_CR0_READ_SHADOW    0x00006004u
#define VMX_CONTROL_CR4_READ_SHADOW    0x00006006u
#define VMX_CONTROL_PPC_HLT_EXITING    (1u << 7)
#define VMX_GUEST_CR0_PE               (1u << 0)
#define VMX_GUEST_CR0_PG               (1u << 31)
#define VMX_GUEST_CR4_PSE              (1u << 4)
#define VMX_GUEST_CR4_PAE              (1u << 5)
#define VMX_GUEST_CR4_VMXE             (1u << 13)
#define VMX_GUEST_EFER_LME             (1u << 8)
#define VMX_GUEST_EFER_LMA             (1u << 10)

/* Flat, present, ring-0 long-mode code/data descriptors in VMCS form. */
#define VMX_DATA_ACCESS_RIGHTS         0xc093u
#define VMX_CODE_ACCESS_RIGHTS         0xa09bu
#define VMX_TR_ACCESS_RIGHTS           0x008bu
#define VMX_UNUSABLE_ACCESS_RIGHTS   0x10000u

static seL4_Error vmcs_write(seL4_CPtr vcpu, seL4_Word field, seL4_Word value)
{
    seL4_X86_VCPU_WriteVMCS_t result =
        seL4_X86_VCPU_WriteVMCS((seL4_X86_VCPU)vcpu, field, value);
    return (seL4_Error)result.error;
}

static void report_and_wait(seL4_CPtr endpoint, seL4_Word status,
                            seL4_Word reason, seL4_Word rip,
                            seL4_Word instruction_len)
{
    seL4_SetMR(0, status);
    seL4_SetMR(1, reason);
    seL4_SetMR(2, rip);
    seL4_SetMR(3, instruction_len);
    seL4_Send(endpoint,
              seL4_MessageInfo_new(AOS_X86_VTX_PROOF_LABEL, 0u, 0u, 4u));

    for (;;) {
        seL4_Word badge = 0u;
        (void)seL4_Wait(PD_CNODE_SLOT_SELF_EP, &badge);
    }
}

static seL4_Error write_vmcs_guest_state(seL4_CPtr vcpu,
                                         seL4_Word *failed_field)
{
    static const struct {
        seL4_Word field;
        seL4_Word value;
    } fields[] = {
        { VMX_GUEST_ES_SELECTOR, VTX_GUEST_DATA_SELECTOR },
        { VMX_GUEST_CS_SELECTOR, VTX_GUEST_CODE_SELECTOR },
        { VMX_GUEST_SS_SELECTOR, VTX_GUEST_DATA_SELECTOR },
        { VMX_GUEST_DS_SELECTOR, VTX_GUEST_DATA_SELECTOR },
        { VMX_GUEST_FS_SELECTOR, 0u },
        { VMX_GUEST_GS_SELECTOR, 0u },
        { VMX_GUEST_LDTR_SELECTOR, 0u },
        { VMX_GUEST_TR_SELECTOR, VTX_GUEST_TR_SELECTOR },

        { VMX_GUEST_ES_LIMIT, VTX_GUEST_FLAT_LIMIT },
        { VMX_GUEST_CS_LIMIT, VTX_GUEST_FLAT_LIMIT },
        { VMX_GUEST_SS_LIMIT, VTX_GUEST_FLAT_LIMIT },
        { VMX_GUEST_DS_LIMIT, VTX_GUEST_FLAT_LIMIT },
        { VMX_GUEST_FS_LIMIT, 0u },
        { VMX_GUEST_GS_LIMIT, 0u },
        { VMX_GUEST_LDTR_LIMIT, 0u },
        { VMX_GUEST_TR_LIMIT, 0u },
        { VMX_GUEST_GDTR_LIMIT, VTX_GUEST_GDT_LIMIT },
        { VMX_GUEST_IDTR_LIMIT, 0u },

        { VMX_GUEST_ES_ACCESS_RIGHTS, VMX_DATA_ACCESS_RIGHTS },
        { VMX_GUEST_CS_ACCESS_RIGHTS, VMX_CODE_ACCESS_RIGHTS },
        { VMX_GUEST_SS_ACCESS_RIGHTS, VMX_DATA_ACCESS_RIGHTS },
        { VMX_GUEST_DS_ACCESS_RIGHTS, VMX_DATA_ACCESS_RIGHTS },
        { VMX_GUEST_FS_ACCESS_RIGHTS, VMX_UNUSABLE_ACCESS_RIGHTS },
        { VMX_GUEST_GS_ACCESS_RIGHTS, VMX_UNUSABLE_ACCESS_RIGHTS },
        { VMX_GUEST_LDTR_ACCESS_RIGHTS, VMX_UNUSABLE_ACCESS_RIGHTS },
        { VMX_GUEST_TR_ACCESS_RIGHTS, VMX_TR_ACCESS_RIGHTS },

        { VMX_GUEST_ES_BASE, 0u },
        { VMX_GUEST_CS_BASE, 0u },
        { VMX_GUEST_SS_BASE, 0u },
        { VMX_GUEST_DS_BASE, 0u },
        { VMX_GUEST_FS_BASE, 0u },
        { VMX_GUEST_GS_BASE, 0u },
        { VMX_GUEST_LDTR_BASE, 0u },
        { VMX_GUEST_TR_BASE, 0u },
        { VMX_GUEST_GDTR_BASE, 0u },
        { VMX_GUEST_IDTR_BASE, 0u },

        { VMX_GUEST_DEBUGCTRL, 0u },
        { VMX_GUEST_EFER, VMX_GUEST_EFER_LME | VMX_GUEST_EFER_LMA },
        { VMX_GUEST_CR0, VMX_GUEST_CR0_PE | VMX_GUEST_CR0_PG },
        { VMX_GUEST_CR3, AOS_X86_VTX_GUEST_PML4_GPA },
        { VMX_GUEST_CR4, VMX_GUEST_CR4_PAE },
        { VMX_GUEST_DR7, 0x400u },
        { VMX_GUEST_RSP, 0x8000u },
        { VMX_GUEST_RFLAGS, 0x2u },
        { VMX_GUEST_PENDING_DEBUG, 0u },
        { VMX_GUEST_INTERRUPTABILITY, 0u },
        { VMX_GUEST_ACTIVITY, 0u },
        { VMX_GUEST_SMBASE, 0u },
        { VMX_GUEST_SYSENTER_CS, 0u },
        { VMX_GUEST_SYSENTER_ESP, 0u },
        { VMX_GUEST_SYSENTER_EIP, 0u },

        /*
         * Both supported SDKs initialize IA-32e guest entry on. This proof
         * retains that mode; root supplies its four guest paging pages.
         * Firmware entry modes require separate qualification.
         */
        { VMX_CONTROL_CR0_MASK, VMX_GUEST_CR0_PE | VMX_GUEST_CR0_PG },
        { VMX_CONTROL_CR0_READ_SHADOW, 0u },
        { VMX_CONTROL_CR4_MASK,
          VMX_GUEST_CR4_PSE | VMX_GUEST_CR4_PAE | VMX_GUEST_CR4_VMXE },
        { VMX_CONTROL_CR4_READ_SHADOW, 0u },
    };

    for (uint32_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); i++) {
        seL4_Error err = vmcs_write(vcpu, fields[i].field, fields[i].value);
        if (err != seL4_NoError) {
            *failed_field = fields[i].field;
            return err;
        }
    }

    seL4_VCPUContext registers = {0};
    seL4_Error err = seL4_X86_VCPU_WriteRegisters((seL4_X86_VCPU)vcpu,
                                                   &registers);
    if (err != seL4_NoError) {
        *failed_field = 0u;
    }
    return err;
}

#ifdef AGENTOS_X86_FIRMWARE_MODES
/* The older SDK cannot expose these controls. Never silently run the old
 * long-mode-only proof when firmware-mode qualification was requested. */
#ifndef SEL4_VMENTER_CALL_CONTROL_ENTRY_MR
#error "Firmware entry qualification requires Microkit 2.3.0 VMCS controls"
#endif
#define VMX_CONTROL_ENTRY              0x00004012u
#define VMX_CONTROL_SECONDARY          0x0000401eu
#define VMX_ENTRY_IA32E                (1u << 9)
#define VMX_SECONDARY_EPT              (1u << 1)
#define VMX_SECONDARY_UNRESTRICTED     (1u << 7)

static void mode_field(seL4_CPtr endpoint, seL4_Word field,
                       seL4_Word value, seL4_Word mask)
{
    seL4_Error err = vmcs_write(AOS_GUEST_VCPU_CAP_BASE, field, value);
    if (err != seL4_NoError) {
        report_and_wait(endpoint, AOS_X86_VTX_PROOF_FAIL, field, err, 0u);
    }
    seL4_X86_VCPU_ReadVMCS_t read =
        seL4_X86_VCPU_ReadVMCS(AOS_GUEST_VCPU_CAP_BASE, field);
    if (read.error != seL4_NoError || (read.value & mask) != (value & mask)) {
        report_and_wait(endpoint, AOS_X86_VTX_PROOF_FAIL, field,
                        read.value, read.error);
    }
}

static void qualify_firmware_modes(seL4_CPtr endpoint)
{
    /* VMM-selected entry states, not a firmware payload or guest-driven
     * transition test. The same EPT-owned HLT byte is valid in all modes. */
    for (unsigned mode = 0u; mode < 3u; mode++) {
        seL4_Word failed_field = 0u;
        seL4_Error err = write_vmcs_guest_state(AOS_GUEST_VCPU_CAP_BASE,
                                               &failed_field);
        if (err != seL4_NoError) {
            report_and_wait(endpoint, AOS_X86_VTX_PROOF_FAIL, failed_field,
                            err, mode);
        }
        seL4_X86_VCPU_ReadVMCS_t secondary =
            seL4_X86_VCPU_ReadVMCS(AOS_GUEST_VCPU_CAP_BASE, VMX_CONTROL_SECONDARY);
        seL4_X86_VCPU_ReadVMCS_t entry =
            seL4_X86_VCPU_ReadVMCS(AOS_GUEST_VCPU_CAP_BASE, VMX_CONTROL_ENTRY);
        if (secondary.error != seL4_NoError || entry.error != seL4_NoError) {
            report_and_wait(endpoint, AOS_X86_VTX_PROOF_FAIL,
                            VMX_CONTROL_ENTRY, entry.error, secondary.error);
        }
        mode_field(endpoint, VMX_CONTROL_SECONDARY,
                   secondary.value | VMX_SECONDARY_UNRESTRICTED | VMX_SECONDARY_EPT,
                   VMX_SECONDARY_UNRESTRICTED | VMX_SECONDARY_EPT);
        mode_field(endpoint, VMX_CONTROL_ENTRY,
                   mode == 2u ? entry.value | VMX_ENTRY_IA32E
                              : entry.value & ~VMX_ENTRY_IA32E,
                   VMX_ENTRY_IA32E);
        mode_field(endpoint, VMX_GUEST_EFER,
                   mode == 2u ? VMX_GUEST_EFER_LME | VMX_GUEST_EFER_LMA : 0u,
                   VMX_GUEST_EFER_LME | VMX_GUEST_EFER_LMA);
        mode_field(endpoint, VMX_GUEST_CR0,
                   mode == 0u ? 0u : mode == 1u ? VMX_GUEST_CR0_PE
                                               : VMX_GUEST_CR0_PE | VMX_GUEST_CR0_PG,
                   VMX_GUEST_CR0_PE | VMX_GUEST_CR0_PG);
        mode_field(endpoint, VMX_GUEST_CR4,
                   mode == 2u ? VMX_GUEST_CR4_PAE : 0u, VMX_GUEST_CR4_PAE);
        if (mode != 2u) {
            mode_field(endpoint, VMX_GUEST_CS_ACCESS_RIGHTS,
                       mode == 0u ? 0x009bu : 0xc09bu, 0xffffu);
        }
        if (mode == 0u) {
            /* Real-address segment caches: selector/base zero, 64 KiB limit,
             * byte granularity, 16-bit default operand/address size. */
            static const seL4_Word selectors[] = {
                VMX_GUEST_ES_SELECTOR, VMX_GUEST_CS_SELECTOR,
                VMX_GUEST_SS_SELECTOR, VMX_GUEST_DS_SELECTOR,
                VMX_GUEST_FS_SELECTOR, VMX_GUEST_GS_SELECTOR,
            };
            static const seL4_Word limits[] = {
                VMX_GUEST_ES_LIMIT, VMX_GUEST_CS_LIMIT, VMX_GUEST_SS_LIMIT,
                VMX_GUEST_DS_LIMIT, VMX_GUEST_FS_LIMIT, VMX_GUEST_GS_LIMIT,
            };
            static const seL4_Word rights[] = {
                VMX_GUEST_ES_ACCESS_RIGHTS, VMX_GUEST_SS_ACCESS_RIGHTS,
                VMX_GUEST_DS_ACCESS_RIGHTS, VMX_GUEST_FS_ACCESS_RIGHTS,
                VMX_GUEST_GS_ACCESS_RIGHTS,
            };
            for (unsigned i = 0u; i < sizeof(selectors) / sizeof(selectors[0]); i++) {
                mode_field(endpoint, selectors[i], 0u, 0xffffu);
                mode_field(endpoint, limits[i], 0xffffu, 0xffffffffu);
            }
            for (unsigned i = 0u; i < sizeof(rights) / sizeof(rights[0]); i++) {
                mode_field(endpoint, rights[i], 0x0093u, 0x1ffffu);
            }
        }

#ifdef AGENTOS_X86_FIRMWARE_RESET
        /* Architectural reset starts with a special high CS cache. Keep
         * CR0 mode writes intercepted for later transition emulation. */
        /* Load all guest EFER bits, not only IA-32e mode implied by entry
         * controls. Otherwise host SCE can survive into the reset guest. */
        mode_field(endpoint, VMX_CONTROL_ENTRY,
                   (entry.value & ~VMX_ENTRY_IA32E) | (1u << 15),
                   VMX_ENTRY_IA32E | (1u << 15));
        mode_field(endpoint, VMX_GUEST_CS_SELECTOR, 0xf000u, 0xffffu);
        mode_field(endpoint, VMX_GUEST_CS_BASE, 0xffff0000u, 0xffffffffu);
        mode_field(endpoint, VMX_GUEST_CR0, 0x60000010u,
                   VMX_GUEST_CR0_PE | VMX_GUEST_CR0_PG);
        mode_field(endpoint, VMX_CONTROL_CR0_READ_SHADOW, 0x60000010u, 0xffffffffu);
        const aos_x86_vmenter_entry_t reset_entry = {
            .ip = 0xfff0u, .controls = VMX_CONTROL_PPC_HLT_EXITING,
            .interruption_info = 0u,
        };
        aos_x86_firmware_run(endpoint, reset_entry);
#else
        seL4_SetMR(SEL4_VMENTER_CALL_EIP_MR, AOS_X86_VTX_GUEST_RIP);
#endif
        seL4_SetMR(SEL4_VMENTER_CALL_CONTROL_PPC_MR, VMX_CONTROL_PPC_HLT_EXITING);
        seL4_SetMR(AOS_VMENTER_INTERRUPT_INFO_MR, 0u);
        aos_x86_vmenter_return_t returned = aos_x86_vm_enter();
        seL4_Word rip = returned.words[SEL4_VMENTER_CALL_EIP_MR];
        if (returned.result != SEL4_VMENTER_RESULT_FAULT)
            report_and_wait(endpoint, AOS_X86_VTX_PROOF_FAIL, 0x4e5446u, rip, returned.badge);
        seL4_Word reason = returned.words[SEL4_VMENTER_FAULT_REASON_MR];
        seL4_Word length = returned.words[SEL4_VMENTER_FAULT_INSTRUCTION_LEN_MR];
        if (reason != AOS_X86_VTX_HLT_EXIT_REASON ||
            rip != AOS_X86_VTX_GUEST_RIP || length != AOS_X86_VTX_HLT_INSTRUCTION_LEN) {
            report_and_wait(endpoint, AOS_X86_VTX_PROOF_FAIL, reason, rip, length);
        }
    }
    report_and_wait(endpoint, AOS_X86_VTX_MODES_PASS,
                    AOS_X86_VTX_HLT_EXIT_REASON, AOS_X86_VTX_GUEST_RIP,
                    AOS_X86_VTX_HLT_INSTRUCTION_LEN);
}
#endif

void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver_endpoint)
{
    (void)nameserver_endpoint;

    if (endpoint == seL4_CapNull) {
        for (;;) {
            seL4_Yield();
        }
    }
    endpoint = AOS_X86_VTX_REPORT_CAP;

#ifdef AGENTOS_X86_FIRMWARE_MODES
    qualify_firmware_modes(endpoint);
#endif

    seL4_Word failed_field = 0u;
    seL4_Error err = write_vmcs_guest_state(AOS_GUEST_VCPU_CAP_BASE,
                                             &failed_field);
    if (err != seL4_NoError) {
        report_and_wait(endpoint, AOS_X86_VTX_PROOF_FAIL, failed_field,
                        (seL4_Word)err, 0u);
    }

#ifdef AGENTOS_X86_GUEST_FAULT_PROOF
    const struct { seL4_Word field, value; } fault_fields[]={
        {VMX_GUEST_GDTR_BASE,AOS_X86_FAULT_GUEST_GDT},
        {VMX_GUEST_GDTR_LIMIT,23},
        {VMX_GUEST_IDTR_BASE,AOS_X86_FAULT_GUEST_IDT},
        {VMX_GUEST_IDTR_LIMIT,14*16-1},
        {VMX_GUEST_RSP,AOS_X86_FAULT_GUEST_STACK},
    };
    for (unsigned i=0;i<sizeof(fault_fields)/sizeof(fault_fields[0]);i++) {
        err=vmcs_write(AOS_GUEST_VCPU_CAP_BASE,fault_fields[i].field,fault_fields[i].value);
        if (err) report_and_wait(endpoint,AOS_X86_VTX_PROOF_FAIL,fault_fields[i].field,err,0);
    }
    seL4_Word next_rip=AOS_X86_FAULT_GUEST_ENTRY, info=0;
    for (unsigned attempt=0;attempt<3;attempt++) {
        seL4_SetMR(SEL4_VMENTER_CALL_EIP_MR,next_rip);
        seL4_SetMR(SEL4_VMENTER_CALL_CONTROL_PPC_MR,VMX_CONTROL_PPC_HLT_EXITING);
        seL4_SetMR(AOS_VMENTER_INTERRUPT_INFO_MR,info);
        aos_x86_vmenter_return_t returned=aos_x86_vm_enter();
        seL4_Word rip=returned.words[SEL4_VMENTER_CALL_EIP_MR];
        if (returned.result!=SEL4_VMENTER_RESULT_FAULT)
            report_and_wait(endpoint,AOS_X86_VTX_PROOF_FAIL,0x4e5446u,rip,returned.badge);
        seL4_Word reason=returned.words[SEL4_VMENTER_FAULT_REASON_MR];
        seL4_Word length=returned.words[SEL4_VMENTER_FAULT_INSTRUCTION_LEN_MR];
        if (attempt==2) {
            if (reason==12u && rip==AOS_X86_VTX_GUEST_RIP && length==1u)
                report_and_wait(endpoint,AOS_X86_VTX_GUEST_FAULTS_PASS,reason,rip,length);
            report_and_wait(endpoint,AOS_X86_VTX_PROOF_FAIL,reason,rip,length);
        }
        if (reason!=(attempt ? 32u : 31u) || length!=2u ||
            returned.words[SEL4_VMENTER_FAULT_ECX]!=UINT32_MAX ||
            returned.words[SEL4_VMENTER_FAULT_EAX]!=0x12345678u ||
            returned.words[SEL4_VMENTER_FAULT_EDX]!=0x87654321u)
            report_and_wait(endpoint,AOS_X86_VTX_PROOF_FAIL,reason,rip,length);
        aos_x86_entry_event_t event;
        if (!aos_x86_entry_event(&event,true,true,length,0,2,0))
            report_and_wait(endpoint,AOS_X86_VTX_PROOF_FAIL,0x45564eu,rip,length);
        err=vmcs_write(AOS_GUEST_VCPU_CAP_BASE,0x4018u,event.error_code);
        if (err) report_and_wait(endpoint,AOS_X86_VTX_PROOF_FAIL,0x4018u,rip,err);
        next_rip=rip+event.advance; info=event.interruption_info;
    }
    report_and_wait(endpoint,AOS_X86_VTX_PROOF_FAIL,0x4750u,0,0);
#endif

    /*
     * SysVMEnter updates guest RIP, primary execution controls, and entry
     * interruption info directly from these three message registers.
     */
    seL4_SetMR(SEL4_VMENTER_CALL_EIP_MR, AOS_X86_VTX_GUEST_RIP);
    seL4_SetMR(SEL4_VMENTER_CALL_CONTROL_PPC_MR, VMX_CONTROL_PPC_HLT_EXITING);
    seL4_SetMR(AOS_VMENTER_INTERRUPT_INFO_MR, 0u);

    aos_x86_vmenter_return_t returned = aos_x86_vm_enter();
    seL4_Word rip = returned.words[SEL4_VMENTER_CALL_EIP_MR];
    if (returned.result != SEL4_VMENTER_RESULT_FAULT)
        report_and_wait(endpoint, AOS_X86_VTX_PROOF_FAIL, 0x4e5446u, rip, returned.badge);
    seL4_Word reason = returned.words[SEL4_VMENTER_FAULT_REASON_MR];
    seL4_Word instruction_len =
        returned.words[SEL4_VMENTER_FAULT_INSTRUCTION_LEN_MR];
    if ((reason & 0xffffu) == AOS_X86_VTX_HLT_EXIT_REASON &&
        rip == AOS_X86_VTX_GUEST_RIP &&
        instruction_len == AOS_X86_VTX_HLT_INSTRUCTION_LEN) {
        report_and_wait(endpoint, AOS_X86_VTX_PROOF_PASS, reason, rip,
                        instruction_len);
    }

    report_and_wait(endpoint, AOS_X86_VTX_PROOF_FAIL, reason, rip,
                    instruction_len);
}
