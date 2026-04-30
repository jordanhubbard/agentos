/*
 * main.c — agentOS root task boot sequence
 *
 * This is the first user-mode code executed after the seL4 microkernel
 * completes its own boot.  seL4 places a pointer to the seL4_BootInfo
 * structure in a well-known register, then jumps to the root task's
 * _start entry point (defined at the bottom of this file).
 *
 * Boot sequence (implemented in root_task_main):
 *
 *   1. ut_alloc_init     — seed the untyped memory allocator from BootInfo
 *   2. cap_acct_init     — initialise capability ownership tracking
 *   3. ep_alloc_init     — initialise the endpoint pool (256 slots)
 *   4. per-PD loop       — for each PD in system order:
 *        a. create VSpace
 *        b. allocate + map IPC buffer frame
 *        c. allocate CNode for the PD
 *        d. locate embedded ELF in BootInfo extra regions
 *        e. load ELF into VSpace; receive entry point and stack top
 *        f. create and configure TCB (CSpace + VSpace + IPC buffer)
 *        g. distribute initial endpoint caps into PD's CNode
 *        h. record all new caps in the accounting tree
 *        i. write initial registers and start the PD thread
 *   5. idle loop         — seL4_Yield() forever
 *
 * The nameserver PD is always pd[0] in the system descriptor, so it is
 * started before any other PD and can accept registration calls
 * immediately.
 *
 * No libc.  No microkit.h.  Pure seL4 invocations only.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "boot_info.h"       /* seL4_BootInfo, seL4_Yield, object type constants */
#include "sel4_boot.h"       /* seL4_IRQControl_Get, seL4_IRQHandler_Ack, etc.   */
#include "ut_alloc.h"        /* ut_alloc_init, ut_alloc                          */
#include "pd_vspace.h"       /* pd_vspace_create, pd_vspace_load_elf              */
#include "pd_tcb.h"          /* pd_tcb_create, pd_tcb_set_regs, pd_tcb_start      */
#include "ep_alloc.h"        /* ep_alloc_init, ep_alloc_for_service, ep_mint_badge */
#include "cap_accounting.h"  /* cap_acct_init, cap_acct_record                    */
#include "cap_audit.h"       /* handle_cap_audit, handle_cap_audit_guest,
                                cap_tree_verify_all_pds                            */
#include "system_desc.h"     /* system_desc_t, pd_desc_t, SVC_ID_*, PD_IRQHANDLER_SLOT_BASE */
#include "agentos.h"         /* sel4_dbg_puts                                    */
#include <stdint.h>

/*
 * g_audit_mr_vaddr — virtual address of the shared capability audit memory region.
 *
 * Set during boot from the system descriptor's shmem layout before any caller
 * can invoke OP_CAP_AUDIT or OP_CAP_AUDIT_GUEST.  In the current build this
 * defaults to 0 (no live target); the simulator and hardware builds override
 * it via their respective shmem mapping steps.
 *
 * cap_audit.c references this symbol via `extern uintptr_t g_audit_mr_vaddr`.
 */
uintptr_t g_audit_mr_vaddr = 0u;

/* ── System descriptor selection ─────────────────────────────────────────── */

/*
 * Choose the correct system descriptor at compile time.
 * The x86_64 QEMU target currently shares the QEMU service topology used by
 * the AArch64 target; the hardware-specific VMM setup paths remain guarded
 * separately by architecture checks.
 */
#if defined(BOARD_qemu_virt_aarch64) || defined(__x86_64__)
extern const system_desc_t system_desc_aarch64;
#define SYSTEM_DESC (&system_desc_aarch64)
#else
extern const system_desc_t system_desc_riscv64;
#define SYSTEM_DESC (&system_desc_riscv64)
#endif

/* ── CNode slot layout ────────────────────────────────────────────────────── */

/*
 * The root task's own CNode contains:
 *   Slots 0 .. seL4_NumInitialCaps-1   — kernel well-known caps (fixed)
 *   Slots seL4_NumInitialCaps onwards  — boot caps (user image frames, untypeds)
 *   Slots bi->empty.start onwards      — free for root-task use
 *
 * g_cap_base is set to bi->empty.start after ut_alloc_init so that all
 * PD_SLOT_* calculations fall within the genuinely-free range.
 * This avoids clobbering boot caps that the kernel placed between
 * seL4_NumInitialCaps and bi->empty.start.
 */
static seL4_Word g_cap_base;  /* set to bi->empty.start in root_task_main */
#define CAP_ROOT_INITIAL_BASE  g_cap_base
#define SLOTS_PER_PD           6u    /* CNODE + TCB + VSPACE + IPC_FRAME + SC + NTFN */

/* Slot layout per PD index i (base + i * SLOTS_PER_PD + offset): */
#define PD_SLOT_CNODE(i)      (CAP_ROOT_INITIAL_BASE + (seL4_Word)(i) * SLOTS_PER_PD + 0u)
#define PD_SLOT_TCB(i)        (CAP_ROOT_INITIAL_BASE + (seL4_Word)(i) * SLOTS_PER_PD + 1u)
#define PD_SLOT_VSPACE(i)     (CAP_ROOT_INITIAL_BASE + (seL4_Word)(i) * SLOTS_PER_PD + 2u)
#define PD_SLOT_IPC_FRAME(i)  (CAP_ROOT_INITIAL_BASE + (seL4_Word)(i) * SLOTS_PER_PD + 3u)
#define PD_SLOT_SC(i)         (CAP_ROOT_INITIAL_BASE + (seL4_Word)(i) * SLOTS_PER_PD + 4u)
#define PD_SLOT_NTFN(i)       (CAP_ROOT_INITIAL_BASE + (seL4_Word)(i) * SLOTS_PER_PD + 5u)

/* VMM-private caps installed into linux_vmm/freebsd_vmm CSpaces.
 *
 * These slots deliberately match the fixed libvmm registration offsets used
 * by the VMM PDs.  They are well above the low service/IRQ slots and fit in
 * the VMM PDs' 1024-slot CNodes.
 */
#define VMM_GUEST_TCB_SLOT_BASE   266u
#define VMM_GUEST_VCPU_SLOT_BASE  330u
#define VMM_FAULT_BADGE_BASE      (1ULL << 62)

/* Active PD scheduling defaults.
 *
 * Most agentOS PDs are short-running IPC servers, so the conservative 1% CPU
 * default keeps the system responsive while preventing an accidental busy loop
 * from monopolising the board. VMM PDs and their guest TCBs are different:
 * booting a real guest is CPU-bound between VM exits. Give them enough budget
 * to make E2E boot progress, while retaining a periodic gap for lower-priority
 * infrastructure PDs.
 */
#define PD_DEFAULT_SC_BUDGET_US   10000u
#define PD_DEFAULT_SC_PERIOD_US   1000000u
#define VMM_SC_BUDGET_US          90000u
#define VMM_SC_PERIOD_US          100000u

/*
 * Number of PD slots reserved statically.  We size for SYSTEM_MAX_PDS
 * so the slot allocation is fully determined at compile time and does not
 * depend on a runtime count.
 */
#define EP_POOL_BASE     (CAP_ROOT_INITIAL_BASE + (seL4_Word)SYSTEM_MAX_PDS * SLOTS_PER_PD)
#define EP_POOL_SIZE     256u

/*
 * Virtual address at which the IPC buffer is mapped in each PD's VSpace.
 * Chosen to be above any typical ELF load region (0x10_0000_0000 on AArch64).
 */
#define PD_IPC_BUF_VA    0x0000000010000000UL

/* ut_alloc_init, ut_free_slot_base, ut_advance_slot_cursor are in ut_alloc.h */

/* ── ELF lookup helpers ───────────────────────────────────────────────────── */

/*
 * Embedded PD bundle — linked into root_task.elf by the build system.
 *
 * tools/ld/root_task.ld places a .pd_bundle section with linker symbols
 * __pd_bundle_start / __pd_bundle_end.  xtask gen-pd-bundle writes the
 * bundle data (agentos_img_hdr_t + PD entry table + PD ELFs) and objcopy
 * inserts it into the section before the final image is assembled.
 *
 * On non-AArch64 builds (where root_task.ld is not used) or when the
 * section is empty, __pd_bundle_start == __pd_bundle_end and the bundle
 * path is skipped.
 */
extern const uint8_t __pd_bundle_start[];
extern const uint8_t __pd_bundle_end[];

/* agentos.img header/entry types for the embedded bundle */
#define AGENTOS_IMAGE_MAGIC_BUNDLE  UINT64_C(0x4147454E544F5300)

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t num_pds;
    uint32_t kernel_off;
    uint32_t kernel_len;
    uint32_t root_off;
    uint32_t root_len;
    uint32_t pd_table_off;
    uint8_t  _pad[28];
} __attribute__((packed)) agentos_bundle_hdr_t;

typedef struct {
    char     name[48];
    uint32_t elf_off;
    uint32_t elf_len;
    uint8_t  priority;
    uint8_t  _pad[7];
} __attribute__((packed)) agentos_bundle_pd_entry_t;

/*
 * bundle_name_match — compare a NUL-terminated elf_path against the
 * fixed-width (48-byte) name field of an agentos_bundle_pd_entry_t.
 *
 * The bundle stores bare stem names (e.g. "controller"); the system
 * descriptor uses ".elf"-suffixed paths (e.g. "controller.elf").  Strip
 * any trailing ".elf" from elf_path before comparing.
 */
static uint32_t bundle_name_match(const char *elf_path, const char name[48])
{
    /* Compute path length and optionally strip ".elf" suffix */
    uint32_t path_len = 0u;
    while (path_len < 48u && elf_path[path_len] != '\0') {
        path_len++;
    }
    if (path_len >= 4u &&
        elf_path[path_len - 4u] == '.' &&
        elf_path[path_len - 3u] == 'e' &&
        elf_path[path_len - 2u] == 'l' &&
        elf_path[path_len - 1u] == 'f') {
        path_len -= 4u;
    }

    uint32_t i = 0u;
    while (i < path_len && i < 48u) {
        if (elf_path[i] != name[i]) {
            return 0u;
        }
        i++;
    }
    /* Match: elf_path stem exhausted and bundle name terminated at same pos */
    return (i == path_len && name[i] == '\0') ? 1u : 0u;
}

/*
 * bundle_size — return the byte length of the embedded PD bundle.
 *
 * Uses pointer subtraction rather than a direct comparison so the compiler
 * does not emit a -Wtautological-compare warning when the linker symbols
 * happen to be equal (empty section).
 */
static seL4_Word bundle_size(void)
{
    /* Cast to uintptr_t to perform arithmetic without array-comparison UB */
    uintptr_t start = (uintptr_t)__pd_bundle_start;
    uintptr_t end   = (uintptr_t)__pd_bundle_end;
    return (end > start) ? (seL4_Word)(end - start) : 0u;
}

/*
 * boot_find_elf_in_bundle — search the embedded .pd_bundle section.
 *
 * Returns a pointer to the ELF data on success, NULL if not found or if
 * the bundle is absent / malformed.
 */
static const void *boot_find_elf_in_bundle(const char *elf_path)
{
    if (bundle_size() < sizeof(agentos_bundle_hdr_t)) {
        return (const void *)0;
    }

    const uint8_t *bundle = __pd_bundle_start;
    const agentos_bundle_hdr_t *hdr = (const agentos_bundle_hdr_t *)bundle;

    if (hdr->magic != AGENTOS_IMAGE_MAGIC_BUNDLE) {
        return (const void *)0;
    }

    const agentos_bundle_pd_entry_t *table =
        (const agentos_bundle_pd_entry_t *)(bundle + hdr->pd_table_off);

    for (uint32_t i = 0u; i < hdr->num_pds; i++) {
        if (bundle_name_match(elf_path, table[i].name)) {
            return (const void *)(bundle + table[i].elf_off);
        }
    }

    return (const void *)0;
}

/*
 * boot_find_elf — locate an embedded ELF image.
 *
 * Search order:
 *   1. Embedded .pd_bundle section (AArch64: PD ELFs baked into root_task.elf)
 *   2. seL4 extra BootInfo region (legacy / non-AArch64 path)
 *
 * Returns a pointer to the start of the ELF data on success, or NULL if
 * no matching chunk is found.  pd_vspace_load_elf handles NULL gracefully
 * by returning an error.
 */
static const void *boot_find_elf(const seL4_BootInfo *bi, const char *elf_path)
{
    /* 1. Try embedded PD bundle */
    const void *from_bundle = boot_find_elf_in_bundle(elf_path);
    if (from_bundle) {
        return from_bundle;
    }

    /* 2. Fall back to seL4 extra BootInfo scan */
    if (!bi || !elf_path || bi->extraLen == 0u) {
        return (const void *)0;
    }

    /*
     * The extra BootInfo data begins at the first address past the
     * BootInfo frame.  The canonical BootInfo frame size is one page
     * (4096 bytes) on all supported architectures.
     */
    const uint8_t *p   = (const uint8_t *)bi + 4096u;
    const uint8_t *end = p + bi->extraLen;

    while (p + sizeof(seL4_BootInfoHeader) <= end) {
        const seL4_BootInfoHeader *hdr = (const seL4_BootInfoHeader *)p;

        if (hdr->len < sizeof(seL4_BootInfoHeader)) {
            break;  /* malformed chunk — stop scanning */
        }

        if (hdr->id == AGENTOS_BOOTINFO_HEADER_ELF &&
            hdr->len >= sizeof(seL4_BootInfoHeader) + sizeof(agentos_elf_region_t)) {

            const agentos_elf_region_t *er =
                (const agentos_elf_region_t *)(p + sizeof(seL4_BootInfoHeader));

            /* Compare PD name (NUL-terminated, bounded) */
            uint32_t match = 1u;
            uint32_t j     = 0u;
            while (j < sizeof(er->pd_name) && (elf_path[j] != '\0' || er->pd_name[j] != '\0')) {
                if (elf_path[j] != er->pd_name[j]) {
                    match = 0u;
                    break;
                }
                j++;
            }

            if (match) {
                /*
                 * ELF data starts immediately after the agentos_elf_region_t
                 * descriptor.  The elf_offset field provides an additional
                 * displacement from the start of the extra BI region for
                 * loaders that place all blobs in a contiguous area.
                 */
                return (const void *)((const uint8_t *)bi + 4096u + er->elf_offset);
            }
        }

        /* Advance to next chunk (round up to word alignment) */
        seL4_Word next = (hdr->len + (seL4_Word)(sizeof(seL4_Word) - 1u)) &
                         ~(seL4_Word)(sizeof(seL4_Word) - 1u);
        p += next;
    }

    return (const void *)0;
}

/*
 * boot_elf_size_in_bundle — return the ELF size from the embedded PD bundle.
 *
 * Returns 0 if the bundle is absent or the PD is not found.
 */
static seL4_Word boot_elf_size_in_bundle(const char *elf_path)
{
    if (bundle_size() < sizeof(agentos_bundle_hdr_t)) {
        return 0u;
    }

    const uint8_t *bundle = __pd_bundle_start;
    const agentos_bundle_hdr_t *hdr = (const agentos_bundle_hdr_t *)bundle;

    if (hdr->magic != AGENTOS_IMAGE_MAGIC_BUNDLE) {
        return 0u;
    }

    const agentos_bundle_pd_entry_t *table =
        (const agentos_bundle_pd_entry_t *)(bundle + hdr->pd_table_off);

    for (uint32_t i = 0u; i < hdr->num_pds; i++) {
        if (bundle_name_match(elf_path, table[i].name)) {
            return (seL4_Word)table[i].elf_len;
        }
    }

    return 0u;
}

/*
 * boot_elf_size — return the ELF image size for a named PD.
 *
 * Checks the embedded bundle first, then the seL4 extra BootInfo region.
 * Returns 0 if the PD is not found.
 */
static seL4_Word boot_elf_size(const seL4_BootInfo *bi, const char *elf_path)
{
    /* 1. Try embedded PD bundle */
    seL4_Word sz = boot_elf_size_in_bundle(elf_path);
    if (sz != 0u) {
        return sz;
    }

    /* 2. Fall back to seL4 extra BootInfo scan */
    if (!bi || !elf_path || bi->extraLen == 0u) {
        return 0u;
    }

    const uint8_t *p   = (const uint8_t *)bi + 4096u;
    const uint8_t *end = p + bi->extraLen;

    while (p + sizeof(seL4_BootInfoHeader) <= end) {
        const seL4_BootInfoHeader *hdr = (const seL4_BootInfoHeader *)p;

        if (hdr->len < sizeof(seL4_BootInfoHeader)) {
            break;
        }

        if (hdr->id == AGENTOS_BOOTINFO_HEADER_ELF &&
            hdr->len >= sizeof(seL4_BootInfoHeader) + sizeof(agentos_elf_region_t)) {

            const agentos_elf_region_t *er =
                (const agentos_elf_region_t *)(p + sizeof(seL4_BootInfoHeader));

            uint32_t match = 1u;
            uint32_t j     = 0u;
            while (j < sizeof(er->pd_name) && (elf_path[j] != '\0' || er->pd_name[j] != '\0')) {
                if (elf_path[j] != er->pd_name[j]) {
                    match = 0u;
                    break;
                }
                j++;
            }

            if (match) {
                return er->elf_size;
            }
        }

        seL4_Word next = (hdr->len + (seL4_Word)(sizeof(seL4_Word) - 1u)) &
                         ~(seL4_Word)(sizeof(seL4_Word) - 1u);
        p += next;
    }

    return 0u;
}

/* ── IRQ capability setup ─────────────────────────────────────────────────── */

static void dbg_puts(const char *s);
static void dbg_hex(seL4_Word v);

/*
 * boot_setup_irqs — bind hardware IRQ handler caps into a PD's CNode.
 *
 * Called once per PD after its CNode and TCB are created.  For each entry in
 * pd->irqs[], the root task calls seL4_IRQControl_Get to obtain an IRQ handler
 * capability from the kernel and places it into the PD's CNode at slot
 * (PD_IRQHANDLER_SLOT_BASE + i).
 *
 * The PD then references these caps by their known slot offsets:
 *   seL4_CPtr irq_cap = PD_IRQHANDLER_SLOT_BASE + i;
 *   seL4_IRQHandler_Ack(irq_cap);   // after handling the IRQ
 *
 * Parameters:
 *   pd               PD descriptor (contains irq_count and irqs[])
 *   pd_cnode         capability to the PD's own CNode (in root task's CSpace)
 *   irq_control_cap  seL4_CapIRQControl - the kernel's IRQ control capability
 *   pd_cnode_depth   radix of pd_cnode (pd->cnode_size_bits)
 */
static void boot_setup_irqs(const pd_desc_t *pd,
                             seL4_CPtr        pd_cnode,
                             seL4_CPtr        irq_control_cap,
                             seL4_Word        pd_cnode_depth,
                             seL4_CPtr        notification_cap)
{
    for (uint8_t i = 0u; i < pd->irq_count; i++) {
        const irq_desc_t *d = &pd->irqs[i];

        /* Destination slot in the PD's own CNode */
        seL4_Word dest_slot = (seL4_Word)PD_IRQHANDLER_SLOT_BASE + (seL4_Word)i;
        seL4_Word root_irq_slot = ut_alloc_slot();
        if (root_irq_slot == seL4_CapNull) {
            dbg_puts("[rt] WARN: no slot for IRQ handler cap\n");
            continue;
        }

        seL4_Error err = seL4_IRQControl_Get(
            irq_control_cap,
            (seL4_Word)d->irq_number,
            seL4_CapInitThreadCNode,
            root_irq_slot,
            64u);
        if (err != seL4_NoError) {
            dbg_puts("[rt] WARN: IRQControl_Get failed irq=");
            dbg_hex((seL4_Word)d->irq_number);
            dbg_puts(" err=");
            dbg_hex((seL4_Word)err);
            dbg_puts("\n");
            continue;
        }

        err = seL4_CNode_Copy(pd_cnode,
                              dest_slot,
                              (uint8_t)pd_cnode_depth,
                              seL4_CapInitThreadCNode,
                              root_irq_slot,
                              64u,
                              seL4_AllRights);
        if (err != seL4_NoError) {
            dbg_puts("[rt] WARN: IRQ cap copy failed irq=");
            dbg_hex((seL4_Word)d->irq_number);
            dbg_puts(" err=");
            dbg_hex((seL4_Word)err);
            dbg_puts("\n");
            continue;
        }

        if (notification_cap != seL4_CapNull) {
            seL4_Word badged_ntfn_slot = ut_alloc_slot();
            if (badged_ntfn_slot == seL4_CapNull) {
                dbg_puts("[rt] WARN: no slot for badged IRQ notification\n");
                continue;
            }

            err = seL4_CNode_Mint(seL4_CapInitThreadCNode,
                                  badged_ntfn_slot,
                                  64u,
                                  seL4_CapInitThreadCNode,
                                  notification_cap,
                                  64u,
                                  seL4_AllRights,
                                  (seL4_Word)d->ntfn_badge);
            if (err != seL4_NoError) {
                dbg_puts("[rt] WARN: IRQ notification mint failed irq=");
                dbg_hex((seL4_Word)d->irq_number);
                dbg_puts(" err=");
                dbg_hex((seL4_Word)err);
                dbg_puts("\n");
                continue;
            }

            err = seL4_IRQHandler_SetNotification((seL4_CPtr)root_irq_slot,
                                                  (seL4_CPtr)badged_ntfn_slot);
            if (err != seL4_NoError) {
                dbg_puts("[rt] WARN: IRQ SetNotification failed irq=");
                dbg_hex((seL4_Word)d->irq_number);
                dbg_puts(" err=");
                dbg_hex((seL4_Word)err);
                dbg_puts("\n");
                continue;
            }
        }

        /*
         * Log failures but do not abort boot: a missing IRQ handler cap means
         * the PD will receive seL4_InvalidCapability when it calls
         * seL4_IRQHandler_Ack(), which is recoverable.
         */
        (void)err;
    }
}

/* ── Direct UART output (PL011, QEMU virt AArch64) ──────────────────────── */

/*
 * PL011 UART mapped at AGENTOS_UART_VA in the root task's VSpace.
 * Initialised in root_task_main after ut_alloc_init and slot-cursor advance.
 * Before init: dbg_puts falls back to sel4_dbg_puts (no-op on release kernel).
 */
#define AGENTOS_UART_PA  0x09000000UL  /* PL011 UART0 physical address on QEMU virt */
#define AGENTOS_UART_VA  0x10001000UL  /* VA in root + controller VSpaces           */

/* QEMU virt GICv2 virtual CPU interface.
 *
 * The guest DTB exposes the GIC CPU interface at IPA 0x08010000. seL4/libvmm
 * expects the real GIC vCPU interface frame at PA 0x08040000 to be mapped at
 * that IPA in the guest execution VSpace, matching libvmm's Microkit example.
 */
#define GIC_VCPU_IF_PA   0x08040000UL
#define GIC_VCPU_IF_VA   0x08010000UL

/* QEMU virt virtio-mmio transports.
 *
 * Slots are 0x200-byte windows inside the 4 KB page at 0x0A000000. The VMM
 * guest sees that page at the same IPA for passthrough probing; cc_pd maps a
 * copy of the same frame at its private VA to drive slot 2 for the host relay.
 */
#define VIRTIO_MMIO_PAGE_PA  0x0A000000UL
#define VIRTIO_MMIO_PAGE_VA  0x0A000000UL
#define FREEBSD_FW_CFG_PAGE_PA  0x09020000UL
#define FREEBSD_FW_CFG_PAGE_VA  0x09020000UL
#define FREEBSD_VIRTIO_MMIO_BUS31_PAGE_PA  0x0A003000UL
#define FREEBSD_VIRTIO_MMIO_BUS31_PAGE_VA  0x0A003000UL

/* VirtIO serial device for cc_pd ↔ host socket bridge.
 * QEMU flags: -device virtio-serial-device,bus=virtio-mmio-bus.2,id=vser0
 *             -device virtserialport,bus=vser0.0,chardev=cc_pd_char,name=cc.0
 * virtio-mmio-bus.2 = PA 0x0A000400, inside the first virtio-mmio page (PA 0x0A000000). */
#define CC_PD_VIRTIO_VA       0x10002000UL  /* VA in cc_pd's VSpace for this device page */
#define CC_PD_STARTUP_VA      0x10003000UL  /* VA in cc_pd's VSpace for startup record   */
#define CC_PD_UART_DBG_VA     0x10004000UL  /* VA in cc_pd's VSpace for debug UART0      */
#define RT_VQ_SCRATCH_VA      0x60000000UL  /* Root-task scratch VA to write startup PAs */

/* Frame cap in root task CNode; seL4_CNode_Copy'd per VSpace that needs output. */
static seL4_CPtr g_uart_frame_cap = seL4_CapNull;
static seL4_CPtr g_virtio_mmio_frame_cap = seL4_CapNull;
static seL4_CPtr g_freebsd_fw_cfg_frame_cap = seL4_CapNull;
static seL4_CPtr g_freebsd_virtio31_frame_cap = seL4_CapNull;

static volatile uint32_t *g_uart_dr;  /* PL011 UARTDR (offset 0x00) */
static volatile uint32_t *g_uart_fr;  /* PL011 UARTFR (offset 0x18) */

#if defined(__x86_64__)
#define X86_COM1_PORT  0x03F8u
static seL4_CPtr g_x86_com1_cap = seL4_CapNull;

static void x86_com1_out(uint16_t port, uint8_t value)
{
    if (g_x86_com1_cap != seL4_CapNull) {
        (void)seL4_X86_IOPort_Out8((seL4_X86_IOPort)g_x86_com1_cap,
                                   (seL4_Word)port,
                                   (seL4_Word)value);
    }
}

static uint8_t x86_com1_in(uint16_t port)
{
    seL4_X86_IOPort_In8_t r =
        seL4_X86_IOPort_In8((seL4_X86_IOPort)g_x86_com1_cap, port);
    if (r.error != seL4_NoError) {
        return 0x20u;
    }
    return r.result;
}

static void x86_com1_putc(char c)
{
    for (uint32_t spin = 0u; spin < 100000u; spin++) {
        if ((x86_com1_in((uint16_t)(X86_COM1_PORT + 5u)) & 0x20u) != 0u) {
            break;
        }
    }
    x86_com1_out(X86_COM1_PORT, (uint8_t)c);
}

static seL4_Word platform_debug_init(void)
{
    seL4_Word slot = ut_alloc_slot();
    if (slot == seL4_CapNull) {
        return 0u;
    }

    seL4_Error err = seL4_X86_IOPortControl_Issue(
        (seL4_X86_IOPortControl)seL4_CapIOPortControl,
        X86_COM1_PORT,
        X86_COM1_PORT + 7u,
        seL4_CapInitThreadCNode,
        slot,
        64u);
    if (err != seL4_NoError) {
        return 1u;  /* slot consumed */
    }

    g_x86_com1_cap = (seL4_CPtr)slot;
    x86_com1_out((uint16_t)(X86_COM1_PORT + 1u), 0x00u); /* disable IRQs */
    x86_com1_out((uint16_t)(X86_COM1_PORT + 3u), 0x80u); /* divisor latch */
    x86_com1_out((uint16_t)(X86_COM1_PORT + 0u), 0x03u); /* 38400 baud */
    x86_com1_out((uint16_t)(X86_COM1_PORT + 1u), 0x00u);
    x86_com1_out((uint16_t)(X86_COM1_PORT + 3u), 0x03u); /* 8N1 */
    x86_com1_out((uint16_t)(X86_COM1_PORT + 2u), 0xC7u); /* FIFO */
    x86_com1_out((uint16_t)(X86_COM1_PORT + 4u), 0x0Bu); /* DTR/RTS */
    return 1u;
}
#else
static seL4_Word platform_debug_init(void)
{
    return 0u;
}
#endif

static void dbg_puts(const char *s)
{
#if defined(__x86_64__)
    if (g_x86_com1_cap != seL4_CapNull) {
        for (; *s; s++) {
            x86_com1_putc(*s);
        }
        return;
    }
#endif
    if (!g_uart_dr) {
        return;  /* UART not yet mapped; silent before step 3.5 */
    }
    for (; *s; s++) {
        while (*g_uart_fr & (1u << 5)) {}  /* spin while TX FIFO full */
        *g_uart_dr = (uint32_t)(uint8_t)*s;
    }
}

/* ── Main boot sequence ───────────────────────────────────────────────────── */

static void dbg_hex(seL4_Word v)
{
    const char hex[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex[(v >> (60 - i * 4)) & 0xf];
    buf[18] = '\0';
    dbg_puts(buf);
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

#if defined(__aarch64__)
static seL4_Error map_vmm_guest_ram_identity(seL4_CPtr vspace,
                                              seL4_Word  guest_pa,
                                              size_t     size)
{
    const seL4_Word large_page = (seL4_Word)1u << seL4_ARCH_LargePageBits;

    if ((guest_pa & (large_page - 1u)) != 0u ||
        (((seL4_Word)size) & (large_page - 1u)) != 0u) {
        return seL4_InvalidArgument;
    }

    for (seL4_Word off = 0u; off < (seL4_Word)size; off += large_page) {
        seL4_CPtr frame = seL4_CapNull;
        seL4_Error err = ut_alloc_device_cap_typed(guest_pa + off,
                                                   (uint32_t)seL4_ARCH_LargePageObject,
                                                   (uint8_t)seL4_ARCH_LargePageBits,
                                                   &frame);
        if (err != seL4_NoError) {
            return err;
        }

        err = pd_vspace_map_device_frame(vspace, frame, guest_pa + off);
        if (err != seL4_NoError) {
            return err;
        }
    }

    return seL4_NoError;
}

static seL4_Error setup_vmm_guest_vcpu(const pd_desc_t *pd,
                                        uint32_t         pd_index,
                                        seL4_CPtr        pd_cnode,
                                        seL4_CPtr        vspace,
                                        seL4_CPtr        ipc_buf_cap,
                                        seL4_Word        ipc_buf_va,
                                        seL4_CPtr        self_ep,
                                        const seL4_BootInfo *bi)
{
    if (!name_eq(pd->name, "linux_vmm") && !name_eq(pd->name, "freebsd_vmm")) {
        return seL4_NoError;
    }

    if (self_ep == seL4_CapNull) {
        dbg_puts("[rt] VMM guest setup skipped: missing self endpoint\n");
        return seL4_InvalidCapability;
    }

    seL4_Word guest_tcb_slot = ut_alloc_slot();
    seL4_Word guest_vcpu_slot = ut_alloc_slot();
    seL4_Word guest_fault_ep_slot = ut_alloc_slot();
    if (guest_tcb_slot == seL4_CapNull ||
        guest_vcpu_slot == seL4_CapNull ||
        guest_fault_ep_slot == seL4_CapNull) {
        dbg_puts("[rt] VMM guest setup failed: no root CNode slots\n");
        return seL4_NotEnoughMemory;
    }

    seL4_Error err = ut_alloc(seL4_TCBObject,
                               0u,
                               seL4_CapInitThreadCNode,
                               guest_tcb_slot,
                               64u);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest TCB alloc err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }

    err = ut_alloc(seL4_ARM_VCPUObject,
                   0u,
                   seL4_CapInitThreadCNode,
                   guest_vcpu_slot,
                   64u);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest VCPU alloc err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }

    seL4_Word cspace_root_data =
        (seL4_Word)(seL4_WordBits - (uint32_t)pd->cnode_size_bits);
    err = seL4_TCB_Configure((seL4_CPtr)guest_tcb_slot,
                              pd_cnode,
                              cspace_root_data,
                              vspace,
                              0u,
                              ipc_buf_va,
                              ipc_buf_cap);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest TCB configure err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }

#ifdef CONFIG_KERNEL_MCS
    seL4_Word guest_sc_slot = ut_alloc_slot();
    if (guest_sc_slot == seL4_CapNull) {
        dbg_puts("[rt] VMM guest SC slot alloc failed\n");
        return seL4_NotEnoughMemory;
    }

    err = ut_alloc(seL4_SchedContextObject,
                   seL4_MinSchedContextBits,
                   seL4_CapInitThreadCNode,
                   guest_sc_slot,
                   64u);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest SC alloc err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }

    err = seL4_SchedControl_ConfigureFlags(
              bi->schedcontrol.start,
              (seL4_SchedContext)guest_sc_slot,
              VMM_SC_BUDGET_US,
              VMM_SC_PERIOD_US,
              0u,
              0u,
              0u);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest SC configure err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }

    err = ep_mint_badge(self_ep,
                        (seL4_Word)(VMM_FAULT_BADGE_BASE | 0u),
                        seL4_CapInitThreadCNode,
                        guest_fault_ep_slot,
                        64u);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest fault EP mint err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }

    err = seL4_TCB_SetSchedParams((seL4_CPtr)guest_tcb_slot,
                                  seL4_CapInitThreadTCB,
                                  255u,
                                  (seL4_Word)pd->priority,
                                  (seL4_CPtr)guest_sc_slot,
                                  (seL4_CPtr)guest_fault_ep_slot);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest SetSchedParams err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }
#else
    err = seL4_TCB_SetPriority((seL4_CPtr)guest_tcb_slot,
                               seL4_CapInitThreadTCB,
                               (seL4_Word)pd->priority);
    if (err != seL4_NoError) {
        return err;
    }
#endif

    err = seL4_ARM_VCPU_SetTCB((seL4_ARM_VCPU)guest_vcpu_slot,
                               (seL4_TCB)guest_tcb_slot);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest VCPU bind err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }

    err = seL4_CNode_Copy(pd_cnode,
                          VMM_GUEST_TCB_SLOT_BASE,
                          (uint8_t)pd->cnode_size_bits,
                          seL4_CapInitThreadCNode,
                          guest_tcb_slot,
                          64u,
                          seL4_AllRights);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest TCB copy err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }

    err = seL4_CNode_Copy(pd_cnode,
                          VMM_GUEST_VCPU_SLOT_BASE,
                          (uint8_t)pd->cnode_size_bits,
                          seL4_CapInitThreadCNode,
                          guest_vcpu_slot,
                          64u,
                          seL4_AllRights);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest VCPU copy err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }

    cap_acct_record(seL4_CapNull, (seL4_CPtr)guest_tcb_slot,
                    seL4_TCBObject, pd_index, pd->name);
    cap_acct_record(seL4_CapNull, (seL4_CPtr)guest_vcpu_slot,
                    seL4_ARM_VCPUObject, pd_index, pd->name);
#ifdef CONFIG_KERNEL_MCS
    cap_acct_record(seL4_CapNull, (seL4_CPtr)guest_sc_slot,
                    seL4_SchedContextObject, pd_index, pd->name);
#endif

    dbg_puts("[rt] VMM guest caps installed tcb=");
    dbg_hex((seL4_Word)guest_tcb_slot);
    dbg_puts(" vcpu=");
    dbg_hex((seL4_Word)guest_vcpu_slot);
    dbg_puts("\n");
    return seL4_NoError;
}
#endif

void root_task_main(const seL4_BootInfo *bi)
{
    /*
     * seL4 sets bi->ipcBuffer = ui_v_reg_end (the raw end of the ELF's virtual
     * address region) WITHOUT rounding down to a page boundary first — the same
     * pattern as bi_frame_vptr (fixed in _rt_start).  The IPC buffer FRAME is
     * mapped at floor(bi->ipcBuffer, PAGE_SIZE); the seL4_IPCBuffer struct is at
     * offset 0 within that frame.  Using the unaligned value shifts every
     * caps_or_badges / msg write by the misaligned offset (e.g. 0x268), so the
     * kernel reads stale zeros from the IPC buffer and reports
     * "Untyped Retype: Destination cap invalid or read-only" for every Retype
     * call that passes an extra cap.
     */
    seL4_IPCBuffer *ipc_buf =
        (seL4_IPCBuffer *)((seL4_Word)bi->ipcBuffer & ~(seL4_Word)0xFFF);
    seL4_SetIPCBuffer(ipc_buf);

    /* ── Step 1: Initialise untyped memory allocator ──────────────────────── */
    ut_alloc_init(bi);
    seL4_Word pre_reserved_slots = platform_debug_init();

    /* Read TPIDR_EL0 on AArch64; other architectures leave this diagnostic 0. */
#if defined(__aarch64__)
    seL4_Word tpidr_val;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tpidr_val));
#else
    seL4_Word tpidr_val = 0u;
#endif

    dbg_puts("[rt] root_task_main: bi=");
    dbg_hex((seL4_Word)bi);
    dbg_puts(" ipcBuffer=");
    dbg_hex((seL4_Word)bi->ipcBuffer);
    dbg_puts(" ipc_buf=");
    dbg_hex((seL4_Word)ipc_buf);
    dbg_puts(" TPIDR_EL0=");
    dbg_hex(tpidr_val);
    dbg_puts(" extraLen=");
    dbg_hex(bi->extraLen);
    dbg_puts("\n");

    dbg_puts("[rt] empty.start=");
    dbg_hex(bi->empty.start);
    dbg_puts(" empty.end=");
    dbg_hex(bi->empty.end);
    dbg_puts("\n");
    dbg_puts("[rt] untyped.start=");
    dbg_hex(bi->untyped.start);
    dbg_puts(" untyped.end=");
    dbg_hex(bi->untyped.end);
    dbg_puts("\n");
    dbg_puts("[rt] cnodeSizeBits=");
    dbg_hex(bi->initThreadCNodeSizeBits);
    dbg_puts("\n");

    dbg_puts("[rt] ut_alloc_init ok\n");

    /*
     * Set the static cap slot base to bi->empty.start (the first truly-free
     * slot) and reserve static slots for per-PD objects and the EP pool so
     * that subsequent ut_alloc_cap() calls start AFTER them.
     */
    g_cap_base = ut_free_slot_base() + pre_reserved_slots;
    ut_advance_slot_cursor((seL4_Word)SYSTEM_MAX_PDS * SLOTS_PER_PD + EP_POOL_SIZE);
    dbg_puts("[rt] g_cap_base=");
    dbg_hex(g_cap_base);
    dbg_puts(" slot_cur_after_advance=");
    dbg_hex(g_cap_base + (seL4_Word)SYSTEM_MAX_PDS * SLOTS_PER_PD + EP_POOL_SIZE);
    dbg_puts("\n");

    /* ── Step 2: Initialise capability accounting ─────────────────────────── */
    cap_acct_init(bi);
    dbg_puts("[rt] cap_acct_init ok\n");

    /* ── Step 3: Initialise endpoint pool ─────────────────────────────────── */
    /*
     * Reserve EP_POOL_SIZE slots starting immediately after the per-PD
     * object slots.  The endpoint pool has a known base so that IPC buffer
     * frame slots (allocated past the EP pool) do not collide with it.
     */
    ep_alloc_init(seL4_CapInitThreadCNode, EP_POOL_BASE, EP_POOL_SIZE);
    dbg_puts("[rt] ep_alloc_init ok\n");

    /* ── Step 3.5: Map platform debug output ──────────────────────────────── */
#if defined(__x86_64__)
    dbg_puts("[rt] x86 COM1 debug output active\n");
#else
    /* Map PL011 UART MMIO for direct boot output. */
    /*
     * Retype the QEMU virt PL011 device untyped (PA 0x09000000) into a 4 KB
     * frame cap in the root task's CNode, then map it at AGENTOS_UART_VA in
     * the root task's VSpace.  After this, dbg_puts writes directly to the
     * PL011 data register, bypassing seL4_DebugPutChar (which is a no-op in
     * the release kernel).
     */
    {
        seL4_Error uart_err = ut_alloc_device_cap(AGENTOS_UART_PA, &g_uart_frame_cap);
        if (uart_err == seL4_NoError) {
            uart_err = pd_vspace_map_device_frame(seL4_CapInitThreadVSpace,
                                                   g_uart_frame_cap,
                                                   AGENTOS_UART_VA);
            if (uart_err == seL4_NoError) {
                g_uart_dr = (volatile uint32_t *)(AGENTOS_UART_VA + 0x00u);
                g_uart_fr = (volatile uint32_t *)(AGENTOS_UART_VA + 0x18u);
            }
        }
    }
    dbg_puts("[rt] UART mapped, direct PL011 output active\n");
#endif

    {
        seL4_Error virtio_err = ut_alloc_device_cap(VIRTIO_MMIO_PAGE_PA,
                                                    &g_virtio_mmio_frame_cap);
        dbg_puts("[rt] virtio-mmio frame cap err=");
        dbg_hex((seL4_Word)virtio_err);
        dbg_puts(" cap=");
        dbg_hex((seL4_Word)g_virtio_mmio_frame_cap);
        dbg_puts("\n");
    }

    {
        seL4_Error fw_err = ut_alloc_device_cap(FREEBSD_FW_CFG_PAGE_PA,
                                                &g_freebsd_fw_cfg_frame_cap);
        dbg_puts("[rt] freebsd fw_cfg frame cap err=");
        dbg_hex((seL4_Word)fw_err);
        dbg_puts(" cap=");
        dbg_hex((seL4_Word)g_freebsd_fw_cfg_frame_cap);
        dbg_puts("\n");
    }

    {
        seL4_Error v31_err = ut_alloc_device_cap(FREEBSD_VIRTIO_MMIO_BUS31_PAGE_PA,
                                                 &g_freebsd_virtio31_frame_cap);
        dbg_puts("[rt] freebsd virtio-mmio bus31 cap err=");
        dbg_hex((seL4_Word)v31_err);
        dbg_puts(" cap=");
        dbg_hex((seL4_Word)g_freebsd_virtio31_frame_cap);
        dbg_puts("\n");
    }

    /* Temporary: dump device untypeds to diagnose UART1 frame allocation */
    {
        uint32_t n = (uint32_t)(bi->untyped.end - bi->untyped.start);
        for (uint32_t i = 0u; i < n; i++) {
            const seL4_UntypedDesc *d = &bi->untypedList[i];
            if (d->isDevice) {
                dbg_puts("[rt] devUT pa="); dbg_hex(d->paddr);
                dbg_puts(" sz="); dbg_hex(1UL << d->sizeBits);
                dbg_puts("\n");
            }
        }
    }

    /* ── Step 3.6: Allocate global fault endpoint for all PDs ────────────── */
    seL4_CPtr g_fault_ep = seL4_CapNull;
    {
        seL4_Error fe = ut_alloc_cap(seL4_EndpointObject, 0u, &g_fault_ep);
        dbg_puts("[rt] fault_ep=");
        dbg_hex((seL4_Word)g_fault_ep);
        dbg_puts(" fe=");
        dbg_hex((seL4_Word)fe);
        dbg_puts("\n");
    }

    /* ── Step 4: Load and start each PD ───────────────────────────────────── */
    const system_desc_t *sys = SYSTEM_DESC;
    dbg_puts("[rt] starting ");
    dbg_hex((seL4_Word)sys->pd_count);
    dbg_puts(" PDs\n");

    for (uint32_t i = 0u; i < sys->pd_count; i++) {
        const pd_desc_t *pd = &sys->pds[i];

        /* ── 4a: Create VSpace ──────────────────────────────────────────── */
        /*
         * pd_vspace_create takes (pd_cnode, asid_pool).
         * At this point we don't yet have the PD's own CNode; pass the
         * ASID pool cap (seL4_CapInitThreadASIDPool) and the init CNode as
         * the PD CNode placeholder.  The real PD CNode is allocated in 4c
         * and subsequently assigned to the TCB.
         *
         * NOTE: pd_vspace_create internally uses ut_alloc to retype
         * paging structures.  The returned vspace_cap is stored in the
         * root task's CNode at PD_SLOT_VSPACE(i).
         */
        dbg_puts("[rt] pd[");
        dbg_hex((seL4_Word)i);
        dbg_puts("] ");
        dbg_puts(pd->name);
        dbg_puts(": vspace...\n");

        pd_vspace_result_t vr_create =
            pd_vspace_create(seL4_CapInitThreadCNode,
                             seL4_CapInitThreadASIDPool);
        if (vr_create.error != 0) {
            dbg_puts("[rt] pd vspace_create fail err=");
            dbg_hex((seL4_Word)vr_create.error);
            dbg_puts("\n");
            continue;
        }
        seL4_CPtr vspace = vr_create.vspace_cap;

        /* ── 4b: Allocate IPC buffer frame ──────────────────────────────── */
        seL4_Error err = ut_alloc(seL4_ARM_SmallPageObject,
                                   0u /* size_bits: fixed 4 KB page */,
                                   seL4_CapInitThreadCNode,
                                   PD_SLOT_IPC_FRAME(i),
                                   64u /* dest_depth */);
        if (err != seL4_NoError) {
            dbg_puts("[rt] pd ipc_frame alloc fail err=");
            dbg_hex((seL4_Word)err);
            dbg_puts("\n");
            continue;
        }
        seL4_CPtr ipc_frame = PD_SLOT_IPC_FRAME(i);

        /* ── 4c: Allocate CNode for the PD ─────────────────────────────── */
        err = ut_alloc(seL4_CapTableObject,
                        pd->cnode_size_bits,
                        seL4_CapInitThreadCNode,
                        PD_SLOT_CNODE(i),
                        64u);
        if (err != seL4_NoError) {
            dbg_puts("[rt] pd cnode alloc fail err=");
            dbg_hex((seL4_Word)err);
            dbg_puts("\n");
            continue;
        }
        seL4_CPtr pd_cnode = PD_SLOT_CNODE(i);

        /* ── 4d: Locate embedded ELF ────────────────────────────────────── */
        const void *elf_data = boot_find_elf(bi, pd->elf_path);
        seL4_Word   elf_size = boot_elf_size(bi, pd->elf_path);

        dbg_puts("[rt] pd elf ");
        dbg_puts(pd->elf_path);
        dbg_puts(elf_data ? " found" : " NOT FOUND\n");
        if (elf_data) {
            const uint64_t *ewords = (const uint64_t *)elf_data;
            dbg_puts(" @");
            dbg_hex((seL4_Word)elf_data);
            dbg_puts(" sz=");
            dbg_hex(elf_size);
            dbg_puts(" hdr=");
            dbg_hex((seL4_Word)ewords[0]); /* first 8 bytes (ELF magic + class etc) */
            dbg_puts(" e_entry=");
            /* ELF64 e_entry is at offset 24 (bytes 24-31) */
            dbg_hex(((const uint64_t *)elf_data)[3]);
            dbg_puts("\n");
        }

        /* ── 4e: Load ELF into VSpace ───────────────────────────────────── */
        pd_vspace_result_t vr = pd_vspace_load_elf(vspace,
                                                    elf_data,
                                                    (uint32_t)elf_size,
                                                    pd->stack_size);
        if (vr.error != 0) {
            dbg_puts("[rt] pd load_elf fail err=");
            dbg_hex((seL4_Word)vr.error);
            dbg_puts("\n");
            continue;
        }

        /* ── 4f: Create and configure TCB ───────────────────────────────── */
        /*
         * Use the IPC buffer frame cap allocated in 4b, and the IPC buffer
         * virtual address from pd_vspace_load_elf (stored in vr.ipc_buf_va).
         * If the VSpace loader did not map an IPC buffer, fall back to the
         * well-known PD_IPC_BUF_VA constant.
         */
        seL4_Word ipc_buf_va = (vr.ipc_buf_va != 0u) ? vr.ipc_buf_va : (seL4_Word)PD_IPC_BUF_VA;
        seL4_CPtr ipc_buf_cap = (vr.ipc_buf_cap != seL4_CapNull) ? vr.ipc_buf_cap : ipc_frame;

        dbg_puts("[rt] pd_cnode=");
        dbg_hex((seL4_Word)pd_cnode);
        dbg_puts(" vspace=");
        dbg_hex((seL4_Word)vspace);
        dbg_puts(" ipc_va=");
        dbg_hex(ipc_buf_va);
        dbg_puts(" ipc_cap=");
        dbg_hex((seL4_Word)ipc_buf_cap);
        dbg_puts("\n");

        pd_tcb_result_t tr = pd_tcb_create(seL4_CapInitThreadCNode,
                                            PD_SLOT_TCB(i),
                                            vspace,
                                            pd_cnode,
                                            ipc_buf_cap,
                                            ipc_buf_va,
                                            pd->priority,
                                            pd->cnode_size_bits);
        if (tr.error != 0) {
            dbg_puts("[rt] pd tcb_create fail err=");
            dbg_hex((seL4_Word)tr.error);
            dbg_puts("\n");
            continue;
        }
        dbg_puts("[rt] pd TCB ok tcb=");
        dbg_hex((seL4_Word)tr.tcb_cap);
        dbg_puts("\n");

        /* seL4_TCB_SetSpace removed: Configure already set cspace/vspace above.
         * fault_ep is set via seL4_TCB_SetSchedParams in the MCS block below. */

        /* ── 4f.5: Allocate and bind scheduling context (seL4 MCS) ──────── */
        /*
         * seL4 MCS requires every active thread to have a Scheduling Context (SC)
         * object bound to its TCB before seL4_TCB_Resume will make the thread
         * runnable.  Without an SC the thread is "passive" — it can only run
         * when invoked via a protected procedure call that donates the caller's SC.
         *
         * For active PDs we allocate a fresh SC and bind it to the TCB.
         * Normal IPC servers receive a conservative budget. VMM PDs receive
         * a larger budget because guest boot is CPU-bound between VM exits.
         * seL4_Time values are in MICROSECONDS.
         * The SchedControl capability is from bi->schedcontrol.start (CPU 0).
         */
#ifdef CONFIG_KERNEL_MCS
        {
            seL4_Error sc_err = ut_alloc(seL4_SchedContextObject,
                                          seL4_MinSchedContextBits,
                                          seL4_CapInitThreadCNode,
                                          PD_SLOT_SC(i),
                                          64u);
            if (sc_err != seL4_NoError) {
                dbg_puts("[rt] pd sc alloc fail err=");
                dbg_hex((seL4_Word)sc_err);
                dbg_puts("\n");
                continue;
            }

            seL4_Word sc_budget = PD_DEFAULT_SC_BUDGET_US;
            seL4_Word sc_period = PD_DEFAULT_SC_PERIOD_US;
            if (name_eq(pd->name, "linux_vmm") || name_eq(pd->name, "freebsd_vmm")) {
                sc_budget = VMM_SC_BUDGET_US;
                sc_period = VMM_SC_PERIOD_US;
            }

            sc_err = seL4_SchedControl_ConfigureFlags(
                         bi->schedcontrol.start,
                         (seL4_SchedContext)PD_SLOT_SC(i),
                         sc_budget,
                         sc_period,
                         0u,           /* extra_refills */
                         0u,           /* badge */
                         0u);          /* flags */
            if (sc_err != seL4_NoError) {
                dbg_puts("[rt] pd sc configure fail err=");
                dbg_hex((seL4_Word)sc_err);
                dbg_puts("\n");
                continue;
            }

            /*
             * seL4_TCB_SetSchedParams (MCS) — binds the SC, sets priority,
             * MCP, and fault endpoint all in one kernel invocation.
             * This replaces both seL4_SchedContext_Bind and the separate
             * seL4_TCB_SetSpace call that was used to install the fault_ep.
             * caps = [authority, sched_context, fault_ep]
             * MRs  = [mcp, priority]
             */
            sc_err = seL4_TCB_SetSchedParams(
                         tr.tcb_cap,
                         seL4_CapInitThreadTCB, /* authority: root TCB, MCP=255 */
                         255u,                  /* mcp */
                         (seL4_Word)pd->priority,
                         (seL4_CPtr)PD_SLOT_SC(i),
                         g_fault_ep);
            if (sc_err != seL4_NoError) {
                dbg_puts("[rt] pd SetSchedParams fail err=");
                dbg_hex((seL4_Word)sc_err);
                dbg_puts("\n");
                continue;
            }
            dbg_puts("[rt] pd SetSchedParams ok\n");
        }
#endif /* CONFIG_KERNEL_MCS */

        dbg_puts("[rt] pd SC bound, starting\n");

        seL4_CPtr pd_ntfn_cap = seL4_CapNull;
        if (pd->irq_count > 0u) {
            seL4_Error ntfn_err = ut_alloc(seL4_NotificationObject,
                                           seL4_NotificationBits,
                                           seL4_CapInitThreadCNode,
                                           PD_SLOT_NTFN(i),
                                           64u);
            if (ntfn_err != seL4_NoError) {
                dbg_puts("[rt] WARN: notification alloc failed err=");
                dbg_hex((seL4_Word)ntfn_err);
                dbg_puts("\n");
            } else {
                pd_ntfn_cap = (seL4_CPtr)PD_SLOT_NTFN(i);
                ntfn_err = seL4_TCB_BindNotification(tr.tcb_cap, pd_ntfn_cap);
                dbg_puts("[rt] notification bind err=");
                dbg_hex((seL4_Word)ntfn_err);
                dbg_puts("\n");
                if (ntfn_err != seL4_NoError) {
                    pd_ntfn_cap = seL4_CapNull;
                }
            }
        }

        /* ── 4g: Distribute initial endpoint caps into PD's CNode ──────── */
        /*
         * For each endpoint spec in pd->init_eps, look up (or lazily
         * allocate) the service endpoint and mint a badged copy into the
         * PD's CNode slot.
         *
         * Badge encoding: bits[63:48] = service_id, bits[47:32] = pd_index.
         * This allows the server to extract the caller's PD identity from
         * the badge on every IPC.
         */
        for (uint32_t e = 0u; e < pd->init_ep_count; e++) {
            const pd_init_ep_t *ep_spec = &pd->init_eps[e];
            seL4_CPtr service_ep = ep_alloc_for_service(ep_spec->service_id);
            if (service_ep == seL4_CapNull) {
                continue;
            }
            seL4_Word badge = ((uint64_t)ep_spec->service_id << 48u) |
                              ((uint64_t)i                   << 32u);
            ep_mint_badge(service_ep, badge,
                           pd_cnode, ep_spec->cnode_slot,
                           pd->cnode_size_bits);
        }

        /* ── 4g.4: Distribute device MMIO frame caps ────────────────────────
         * For each device_frame_desc_t, find the device untyped covering its
         * physical address, retype it as a 4K page frame, and install the cap
         * directly into the PD's CNode at the specified slot.              */
        for (uint8_t j = 0u; j < pd->device_frame_count; j++) {
            const device_frame_desc_t *df = &pd->device_frames[j];
            seL4_Error df_err = ut_alloc_device_frame(
                (seL4_Word)df->paddr,
                pd_cnode,
                (seL4_Word)df->cnode_slot
            );
            if (df_err != seL4_NoError) {
                dbg_puts("[root] WARN: device frame retype failed: ");
                dbg_puts(df->name);
                dbg_puts("\n");
            }
        }

        /* ── 4g.4.5: Map large anonymous RAM regions into the PD's VSpace ─── */
        /*
         * For each memory_region_desc_t, allocate 2 MB large pages from the
         * untyped pool and map them at [mr->vaddr, mr->vaddr+mr->size) in the
         * PD's VSpace.  Frame caps are retained in the root task's CNode to
         * maintain the mappings.  Used for linux_vmm guest RAM (256 MB).     */
        for (uint8_t j = 0u; j < pd->mr_count; j++) {
            const memory_region_desc_t *mr = &pd->memory_regions[j];
            seL4_Error mr_err;
#if defined(__aarch64__)
            if ((name_eq(pd->name, "linux_vmm") || name_eq(pd->name, "freebsd_vmm")) &&
                name_eq(mr->name, "guest_ram")) {
                /* QEMU virtio passthrough DMAs into guest physical addresses.
                 * Map the guest RAM window from the same physical RAM range
                 * advertised in the guest DTB so QEMU and the VMM share backing
                 * pages for virtqueue descriptors and data buffers. */
                mr_err = map_vmm_guest_ram_identity(vspace,
                                                    (seL4_Word)mr->vaddr,
                                                    (size_t)mr->size);
            } else
#endif
            {
                mr_err = pd_vspace_map_region(
                    vspace,
                    (seL4_Word)mr->vaddr,
                    (size_t)mr->size,
                    (int)mr->writable
                );
            }
            if (mr_err != seL4_NoError) {
                dbg_puts("[root] WARN: memory region map failed: ");
                dbg_puts(mr->name);
                dbg_puts("\n");
            }
        }

        /* ── 4g.4.6: Map UART MMIO into debug-printing PD VSpaces ─────────── */
        /*
         * The controller and current VMM PDs still use direct early debug
         * output at AGENTOS_UART_VA.  We copy the root task's UART frame cap
         * (already mapped in the root VSpace) to a fresh slot and map it into
         * those VSpaces.  seL4_CNode_Copy clears capFMappedASID in the copy,
         * allowing independent re-mapping.
         *
         * The IPC buffer mapping (pd_vspace_load_elf, 0x10000000) already
         * installed the L1 page table covering [0x10000000, 0x11FFFFF], so
         * the map call for 0x10001000 succeeds without any PT retries.
         */
        if (g_uart_frame_cap != seL4_CapNull &&
            (name_eq(pd->name, "controller") ||
             name_eq(pd->name, "linux_vmm") ||
             name_eq(pd->name, "freebsd_vmm"))) {
            seL4_Word uart_copy = ut_alloc_slot();
            dbg_puts("[rt] debug UART: pd=");
            dbg_puts(pd->name);
            dbg_puts(" uart_copy=");
            dbg_hex(uart_copy);
            dbg_puts(" g_uart_frame_cap=");
            dbg_hex((seL4_Word)g_uart_frame_cap);
            dbg_puts("\n");
            if (uart_copy != seL4_CapNull) {
                seL4_Error cp_err = seL4_CNode_Copy(
                    seL4_CapInitThreadCNode, uart_copy,         64u,
                    seL4_CapInitThreadCNode, g_uart_frame_cap,  64u,
                    seL4_AllRights);
                dbg_puts("[rt] debug UART CNode_Copy err=");
                dbg_hex((seL4_Word)cp_err);
                dbg_puts("\n");
                if (cp_err == seL4_NoError) {
                    cp_err = pd_vspace_map_device_frame(vspace,
                                                         (seL4_CPtr)uart_copy,
                                                         AGENTOS_UART_VA);
                    dbg_puts("[rt] debug UART map err=");
                    dbg_hex((seL4_Word)cp_err);
                    dbg_puts("\n");
                }
                if (cp_err != seL4_NoError) {
                    dbg_puts("[rt] WARN: debug UART map failed\n");
                } else {
                    dbg_puts("[rt] debug UART mapped OK\n");
                }
            } else {
                dbg_puts("[rt] WARN: no slot for debug UART copy\n");
            }
        }

        /* ── 4g.4.6b: Map GICv2 vCPU interface for VMM guests ───────────── */
        /*
         * QEMU virt exposes a GICv2 CPU interface to the guest at 0x08010000.
         * On seL4 this is backed by the hardware virtual CPU interface frame
         * at 0x08040000. Without this pass-through mapping Linux faults as
         * soon as it writes GICC_PMR during IRQ setup.
         */
        if (name_eq(pd->name, "linux_vmm") || name_eq(pd->name, "freebsd_vmm")) {
            seL4_CPtr gic_vcpu_cap = seL4_CapNull;
            seL4_Error gic_err = ut_alloc_device_cap(GIC_VCPU_IF_PA, &gic_vcpu_cap);
            if (gic_err == seL4_NoError) {
                gic_err = pd_vspace_map_device_frame(vspace,
                                                      gic_vcpu_cap,
                                                      GIC_VCPU_IF_VA);
            }
            dbg_puts("[rt] VMM GIC vCPU map err=");
            dbg_hex((seL4_Word)gic_err);
            dbg_puts("\n");
        }

        /* ── 4g.4.6c: Map QEMU virtio-mmio transport page for VMM guests ─── */
        /*
         * The guest DTB exposes virtio-mmio slots under 0x0A000000. Map the
         * single backing page into the guest execution VSpace so Linux can
         * probe QEMU's virtio-net and virtio-blk transports directly while
         * IRQ delivery still flows through the VMM/vGIC path.
         */
        if (g_virtio_mmio_frame_cap != seL4_CapNull &&
            (name_eq(pd->name, "linux_vmm") || name_eq(pd->name, "freebsd_vmm"))) {
            seL4_Word virtio_copy = ut_alloc_slot();
            seL4_Error virtio_err = seL4_NotEnoughMemory;
            if (virtio_copy != seL4_CapNull) {
                virtio_err = seL4_CNode_Copy(
                    seL4_CapInitThreadCNode, virtio_copy,              64u,
                    seL4_CapInitThreadCNode, g_virtio_mmio_frame_cap,  64u,
                    seL4_AllRights);
                if (virtio_err == seL4_NoError) {
                    virtio_err = pd_vspace_map_device_frame(vspace,
                                                            (seL4_CPtr)virtio_copy,
                                                            VIRTIO_MMIO_PAGE_VA);
                }
            }
            dbg_puts("[rt] VMM virtio-mmio map err=");
            dbg_hex((seL4_Word)virtio_err);
            dbg_puts("\n");
        }

        if (name_eq(pd->name, "freebsd_vmm")) {
            if (g_freebsd_fw_cfg_frame_cap != seL4_CapNull) {
                seL4_Word fw_copy = ut_alloc_slot();
                seL4_Error fw_err = seL4_NotEnoughMemory;
                if (fw_copy != seL4_CapNull) {
                    fw_err = seL4_CNode_Copy(
                        seL4_CapInitThreadCNode, fw_copy,                    64u,
                        seL4_CapInitThreadCNode, g_freebsd_fw_cfg_frame_cap, 64u,
                        seL4_AllRights);
                    if (fw_err == seL4_NoError) {
                        fw_err = pd_vspace_map_device_frame(vspace,
                                                            (seL4_CPtr)fw_copy,
                                                            FREEBSD_FW_CFG_PAGE_VA);
                    }
                }
                dbg_puts("[rt] FreeBSD fw_cfg map err=");
                dbg_hex((seL4_Word)fw_err);
                dbg_puts("\n");
            }

            if (g_freebsd_virtio31_frame_cap != seL4_CapNull) {
                seL4_Word v31_copy = ut_alloc_slot();
                seL4_Error v31_err = seL4_NotEnoughMemory;
                if (v31_copy != seL4_CapNull) {
                    v31_err = seL4_CNode_Copy(
                        seL4_CapInitThreadCNode, v31_copy,                       64u,
                        seL4_CapInitThreadCNode, g_freebsd_virtio31_frame_cap,    64u,
                        seL4_AllRights);
                    if (v31_err == seL4_NoError) {
                        v31_err = pd_vspace_map_device_frame(vspace,
                                                             (seL4_CPtr)v31_copy,
                                                             FREEBSD_VIRTIO_MMIO_BUS31_PAGE_VA);
                    }
                }
                dbg_puts("[rt] FreeBSD virtio bus31 map err=");
                dbg_hex((seL4_Word)v31_err);
                dbg_puts("\n");
            }
        }

        /* ── 4g.4.7: Set up VirtIO serial transport for cc_pd ───────────────── */
        /*
         * cc_pd uses VirtIO serial (bus.2 = PA 0x0A000400) as its host socket
         * bridge.  QEMU bridges it to build/cc_pd.sock via virtserialport.
         *
         * We map three resources into cc_pd's VSpace:
         *   1. Device page at PA 0x0A000000 (covers virtio-mmio slots 0-7) at
         *      CC_PD_VIRTIO_VA.  cc_pd probes slot 2 (offset +0x400).
         *   2. Three normal frames for VirtIO queue memory (desc/avail/used
         *      rings + TX/RX data buffers).  Each is mapped at VA = PA so cc_pd
         *      can use the pointer value directly as the DMA physical address.
         *   3. A startup record page at CC_PD_STARTUP_VA carrying the three PAs.
         */
        if (name_eq(pd->name, "cc_pd")) {
            /* 1. Allocate + map VirtIO MMIO device page */
            seL4_CPtr cc_virtio_cap = seL4_CapNull;
            {
                seL4_Error ve = seL4_InvalidCapability;
                if (g_virtio_mmio_frame_cap != seL4_CapNull) {
                    seL4_Word cc_virtio_slot = ut_alloc_slot();
                    if (cc_virtio_slot != seL4_CapNull) {
                        ve = seL4_CNode_Copy(
                            seL4_CapInitThreadCNode, cc_virtio_slot,           64u,
                            seL4_CapInitThreadCNode, g_virtio_mmio_frame_cap,  64u,
                            seL4_AllRights);
                        if (ve == seL4_NoError) {
                            cc_virtio_cap = (seL4_CPtr)cc_virtio_slot;
                        }
                    } else {
                        ve = seL4_NotEnoughMemory;
                    }
                }
                if (ve == seL4_NoError && cc_virtio_cap != seL4_CapNull) {
                    ve = pd_vspace_map_device_frame(vspace, cc_virtio_cap, CC_PD_VIRTIO_VA);
                }
                dbg_puts("[rt] cc_pd VirtIO page err=");
                dbg_hex((seL4_Word)ve);
                dbg_puts("\n");
            }

            /* 2. Allocate 3 normal frames for queue structs + TX/RX buffers.
             *    Each mapped at VA = PA (identity) so cc_pd uses ptr as DMA PA. */
            seL4_Word vq_pas[3] = {0u, 0u, 0u};
            for (uint32_t vqp = 0u; vqp < 3u; vqp++) {
                seL4_CPtr vq_cap = seL4_CapNull;
                seL4_Error ve = ut_alloc_cap(seL4_ARM_SmallPageObject, 0u, &vq_cap);
                if (ve == seL4_NoError) {
                    seL4_ARCH_Page_GetAddress_t r = seL4_ARCH_Page_GetAddress(vq_cap);
                    vq_pas[vqp] = r.paddr;
                    ve = pd_vspace_map_device_frame(vspace, vq_cap, vq_pas[vqp]);
                }
                dbg_puts("[rt] cc_pd vq[");
                dbg_hex((seL4_Word)vqp);
                dbg_puts("] pa=");
                dbg_hex(vq_pas[vqp]);
                dbg_puts(" err=");
                dbg_hex((seL4_Word)ve);
                dbg_puts("\n");
            }

            /* 3. Startup record: map in root task, write VQ PAs, remap in cc_pd.
             * A single cap can only be mapped once; unmap from root task before
             * mapping into cc_pd's VSpace so cc_pd can read the PAs at startup. */
            seL4_CPtr cc_start_cap = seL4_CapNull;
            {
                seL4_Error ve = ut_alloc_cap(seL4_ARM_SmallPageObject, 0u, &cc_start_cap);
                if (ve == seL4_NoError) {
                    ve = pd_vspace_map_device_frame(seL4_CapInitThreadVSpace,
                                                    cc_start_cap, RT_VQ_SCRATCH_VA);
                    if (ve == seL4_NoError) {
                        volatile seL4_Word *sp = (volatile seL4_Word *)RT_VQ_SCRATCH_VA;
                        sp[0] = vq_pas[0];
                        sp[1] = vq_pas[1];
                        sp[2] = vq_pas[2];
                        AGENTOS_MEMORY_FENCE();
                        seL4_ARCH_Page_Unmap(cc_start_cap);
                        seL4_Error ve2 = pd_vspace_map_device_frame(vspace, cc_start_cap, CC_PD_STARTUP_VA);
                        dbg_puts("[rt] cc_pd startup PAs written remap_err=");
                        dbg_hex((seL4_Word)ve2);
                        dbg_puts("\n");
                    } else {
                        dbg_puts("[rt] WARN: cc_pd startup page map err=");
                        dbg_hex((seL4_Word)ve);
                        dbg_puts("\n");
                    }
                }
            }

            /* 4. UART0 debug access: copy frame cap + map at CC_PD_UART_DBG_VA
             * so cc_pd can print to the seL4 debug console even without
             * CONFIG_PRINTING (which is disabled in the release SDK). */
            if (g_uart_frame_cap != seL4_CapNull) {
                seL4_Word cc_uart_copy = ut_alloc_slot();
                if (cc_uart_copy != seL4_CapNull) {
                    seL4_Error ce = seL4_CNode_Copy(
                        seL4_CapInitThreadCNode, cc_uart_copy,        64u,
                        seL4_CapInitThreadCNode, g_uart_frame_cap,    64u,
                        seL4_AllRights);
                    if (ce == seL4_NoError) {
                        ce = pd_vspace_map_device_frame(vspace,
                                                        (seL4_CPtr)cc_uart_copy,
                                                        CC_PD_UART_DBG_VA);
                    }
                    dbg_puts("[rt] cc_pd UART map err=");
                    dbg_hex((seL4_Word)ce);
                    dbg_puts("\n");
                }
            }
        }

        /* ── 4g.5: Bind hardware IRQ handler caps into the PD's CNode ─────── */
        /*
         * For each irq_desc_t in pd->irqs[], obtain an IRQ handler cap from
         * the kernel and place it at slot (PD_IRQHANDLER_SLOT_BASE + index)
         * in the PD's CNode.  PDs with irq_count == 0 skip this step.
         */
        if (pd->irq_count > 0u) {
            boot_setup_irqs(pd, pd_cnode,
                            seL4_CapIRQControl,
                            (seL4_Word)pd->cnode_size_bits,
                            pd_ntfn_cap);
        }

        /* ── 4g.6: Allocate MCS reply object at slot AGENTOS_IPC_REPLY_CAP ── */
        /*
         * seL4 MCS seL4_Recv(ep, badge, reply_cap) requires a pre-allocated
         * seL4_ReplyObject in the reply_cap slot.  sel4_ipc.h reserves slot 9
         * (AGENTOS_IPC_REPLY_CAP) for this purpose in every service PD's CNode.
         * Without this, every seL4_Recv call generates a seL4_Fault_CapFault.
         */
#ifdef CONFIG_KERNEL_MCS
        {
            seL4_CPtr reply_slot = ut_alloc_slot();
            dbg_puts("[rt] reply_slot=");
            dbg_hex((seL4_Word)reply_slot);
            dbg_puts("\n");
            if (reply_slot != seL4_CapNull) {
                seL4_Error re = ut_alloc(seL4_ReplyObject,
                                         seL4_ReplyBits,
                                         seL4_CapInitThreadCNode,
                                         reply_slot,
                                         64u);
                dbg_puts("[rt] reply alloc err=");
                dbg_hex((seL4_Word)re);
                dbg_puts("\n");
                if (re == seL4_NoError) {
                    re = seL4_CNode_Copy(
                             pd_cnode,              /* dest CNode */
                             9u,                    /* dest slot (AGENTOS_IPC_REPLY_CAP) */
                             (seL4_Word)pd->cnode_size_bits,
                             seL4_CapInitThreadCNode, /* src CNode */
                             reply_slot,            /* src slot */
                             64u,
                             seL4_AllRights);
                    dbg_puts("[rt] reply copy err=");
                    dbg_hex((seL4_Word)re);
                    dbg_puts("\n");
                }
                if (re != seL4_NoError) {
                    dbg_puts("[rt] WARN: reply obj install failed: ");
                    dbg_puts(pd->name);
                    dbg_puts("\n");
                }
            }
        }
#endif /* CONFIG_KERNEL_MCS */

        /* ── 4h: Record all new caps in the accounting tree ─────────────── */
        cap_acct_record(seL4_CapNull, pd_cnode, seL4_CapTableObject,   i, pd->name);
        cap_acct_record(seL4_CapNull, vspace,   seL4_ARM_VSpaceObject, i, pd->name);
        cap_acct_record(seL4_CapNull, tr.tcb_cap, seL4_TCBObject,       i, pd->name);
        cap_acct_record(seL4_CapNull, ipc_frame,  seL4_ARM_SmallPageObject, i, pd->name);

        /* ── 4i: Mint self endpoint, write registers, and start PD thread ── */
        /*
         * If the PD has a server endpoint (self_svc_id != 0), allocate it
         * and mint an unbadged copy into the PD's CNode at SELF_EP slot.
         * The slot index is passed as arg0 (x0) so pd_main receives it
         * without a nameserver lookup.  arg1 (x1) carries the nameserver
         * endpoint slot so PDs can register on first call.
         */
        seL4_Word self_ep_slot = 0u;
        seL4_CPtr self_ep = seL4_CapNull;
        if (pd->self_svc_id != 0u) {
            self_ep = ep_alloc_for_service((uint16_t)pd->self_svc_id);
            if (self_ep != seL4_CapNull) {
                ep_mint_badge(self_ep, 0u /* unbadged */,
                              pd_cnode, PD_CNODE_SLOT_SELF_EP,
                              pd->cnode_size_bits);
                self_ep_slot = PD_CNODE_SLOT_SELF_EP;
            }
        }

#if defined(__aarch64__)
        if (name_eq(pd->name, "linux_vmm") || name_eq(pd->name, "freebsd_vmm")) {
            seL4_Error vm_err = setup_vmm_guest_vcpu(pd,
                                                      i,
                                                      pd_cnode,
                                                      vspace,
                                                      ipc_buf_cap,
                                                      ipc_buf_va,
                                                      self_ep,
                                                      bi);
            if (vm_err != seL4_NoError) {
                dbg_puts("[rt] WARN: VMM guest cap setup failed for ");
                dbg_puts(pd->name);
                dbg_puts(" err=");
                dbg_hex((seL4_Word)vm_err);
                dbg_puts("\n");
            }
        }
#endif
        {
            dbg_puts("[rt] pd entry=");
            dbg_hex(vr.entry_point);
            dbg_puts(" sp=");
            dbg_hex(vr.stack_top);
            dbg_puts("\n");
            seL4_Error reg_err = pd_tcb_set_regs(tr.tcb_cap,
                                                   vr.entry_point,
                                                   vr.stack_top,
                                                   self_ep_slot,
                                                   (seL4_Word)PD_CNODE_SLOT_NAMESERVER_EP);
            if (reg_err != seL4_NoError) {
                dbg_puts("[rt] pd set_regs fail err=");
                dbg_hex((seL4_Word)reg_err);
                dbg_puts("\n");
            }
            seL4_Error start_err = pd_tcb_start(tr.tcb_cap);
            if (start_err != seL4_NoError) {
                dbg_puts("[rt] pd tcb_start fail err=");
                dbg_hex((seL4_Word)start_err);
                dbg_puts(" tcb=");
                dbg_hex((seL4_Word)tr.tcb_cap);
                dbg_puts("\n");
            } else {
                dbg_puts("[rt] pd started ok\n");
            }
        }
    }

    /* ── Step 4.5: Capability audit baseline ─────────────────────────────── */
    /*
     * After all PDs are started, record the initial capability counts per PD
     * as a regression baseline.  cap_tree_verify_all_pds() is implemented in
     * cap_audit.c; it walks the cap accounting table and logs counts.
     *
     * The OP_CAP_AUDIT and OP_CAP_AUDIT_GUEST handlers are available to the
     * controller PD via the root task's service endpoint.  They are dispatched
     * from the server loop (to be wired in via sel4_server_register once the
     * server loop is introduced in a future sprint).
     *
     * For now, declare the handlers here so the linker confirms they resolve:
     */
    (void)handle_cap_audit;
    (void)handle_cap_audit_guest;

    cap_tree_verify_all_pds();

    dbg_puts("[rt] boot complete — yielding to PDs\n");

    /* ── Step 5: Yield CPU to PDs via IPC block ───────────────────────────── */
    /*
     * Blocking on seL4_Wait is sufficient to yield CPU — the root task enters
     * IPC-waiting state and the scheduler picks the next runnable PD.
     * Do NOT lower priority before blocking: SetPriority on self causes
     * immediate preemption in seL4 MCS (if higher-priority threads are ready),
     * meaning the root task loses CPU before it can reach seL4_Wait, and may
     * not regain it if its SC budget was consumed during init.
     */
    if (g_fault_ep != seL4_CapNull) {
        dbg_puts("[rt] parking on fault_ep\n");
        for (;;) {
            seL4_Word badge = 0u;
            seL4_MessageInfo_t tag = seL4_Wait(g_fault_ep, &badge);
            seL4_Word label = seL4_MessageInfo_get_label(tag);
            dbg_puts("[rt] FAULT label=");
            dbg_hex(label);
            dbg_puts(" badge=");
            dbg_hex(badge);
            dbg_puts("\n    MR0-7(regs):");
            for (int mri = 0; mri <= 7; mri++) {
                dbg_puts(" ");
                dbg_hex(seL4_GetMR(mri));
            }
            dbg_puts("\n    MR8(IP)=");
            dbg_hex(seL4_GetMR(8));
            dbg_puts(" MR9=");
            dbg_hex(seL4_GetMR(9));
            dbg_puts(" MR10=");
            dbg_hex(seL4_GetMR(10));
            dbg_puts(" MR11=");
            dbg_hex(seL4_GetMR(11));
            dbg_puts("\n");
        }
    }

    /* Fallback if fault endpoint unavailable — spin with yield */
    dbg_puts("[rt] WARNING: fault_ep unavail, spinning\n");
    for (;;) {
        seL4_Yield();
    }
}

/* ── Root task entry point ────────────────────────────────────────────────── */

/*
 * _rt_start — seL4 root task C entry point.
 *
 * On AArch64, called from start_aarch64.S after SP is initialized.
 * On RISC-V, _start is this function directly (SP set by seL4 convention).
 * On x86_64, start_x86_64.S installs a bootstrap stack before calling C.
 *
 * seL4 AArch64 boot protocol: BootInfo pointer is in x0 (capRegister).
 * seL4 RISC-V boot protocol:  BootInfo pointer is in a0.
 * seL4 x86_64 boot protocol:  BootInfo pointer is in rdi.
 */
#if defined(__aarch64__)
/*
 * _rt_start — AArch64 C entry from start_aarch64.S.
 *
 * seL4 AArch64 boot protocol: capRegister (x0) = bi_frame_vptr (BootInfo
 * virtual address in root task's VSpace).  start_aarch64.S preserves x0
 * (only touches x9 and sp) before branching here, so the C calling
 * convention delivers seL4's x0 as bi.
 */
void __attribute__((noreturn)) _rt_start(seL4_BootInfo *bi)
{
    /*
     * seL4 computes bi_frame_vptr = ui_v_reg_end + PAGE_SIZE without rounding
     * up first, so bi_frame_vptr may not be page-aligned (e.g. 0x6889c268).
     * The BootInfo frame is always mapped at floor(bi_frame_vptr, PAGE_SIZE)
     * and the struct is written at offset 0 within that frame.  Align down so
     * we read the struct from its true base.
     */
    bi = (seL4_BootInfo *)((seL4_Word)bi & ~(seL4_Word)0xFFF);
    dbg_puts("[rt] bi=");
    dbg_hex((seL4_Word)bi);
    dbg_puts("\n");
    root_task_main(bi);
    __builtin_unreachable();
}
#elif defined(__x86_64__)
void __attribute__((noreturn)) _rt_start_x86_64(seL4_BootInfo *bi)
{
    root_task_main(bi);
    __builtin_unreachable();
}
#else
void __attribute__((noreturn)) _start(seL4_BootInfo *bi)
{
#if !defined(__riscv) && !defined(__x86_64__)
    bi = (seL4_BootInfo *)0;
#endif
    root_task_main(bi);
    __builtin_unreachable();
}
#endif
