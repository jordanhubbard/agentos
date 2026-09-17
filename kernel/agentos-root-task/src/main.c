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
#include "contracts/guest_execution_caps.h"
#if defined(__x86_64__) && defined(AGENTOS_X86_VTX)
#include "contracts/x86_vtx_proof.h"
#endif
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
#include "contracts/cc_contract.h" /* cc_pd VirtIO startup ABI                    */
#include <platform/blk_host_layout.h> /* host block MMIO/shared DMA layout       */
#include <platform/blk_layout.h>      /* shared sDDF block region (VMMs + blk_virt) */
#include <platform/serial_virt_layout.h>
#ifdef AGENTOS_FRAMEBUFFER_TEST
#include <platform/framebuffer.h>
#include <platform/framebuffer_isolation_probe.h>
static seL4_CPtr g_framebuffer_frames[AOS_FB_CLIENTS];
static seL4_CPtr g_framebuffer_arena[AOS_FB_ARENA_FRAMES];
#endif
#include <contracts/serial_virt_contract.h>
#include <contracts/blk_virt_contract.h>
#include <platform/vmm_isolation_probe.h>
#include <platform/native_net_isolation_probe.h>
#include <platform/operator_isolation_probe.h>
#include <platform/log_isolation_probe.h>
#ifdef AGENTOS_LOG_RINGS
#include <platform/log_ring.h>
#ifdef AGENTOS_FRAMEBUFFER_TEST
_Static_assert(PD_CNODE_SLOT_FB_WAIT != AOS_LOG_NOTIFY_CAP &&
               PD_CNODE_SLOT_FB_PEER_NOTIFY > AOS_LOG_NOTIFY_CAP &&
               PD_CNODE_SLOT_FB_PEER_NOTIFY + AOS_FB_CLIENTS <= PD_IRQHANDLER_SLOT_BASE,
               "framebuffer caps must not overlap logs or IRQ handlers");
#endif
#endif
#include <contracts/virtualizer_authority.h>
#include <contracts/net_virt_contract.h>
#if defined(AGENTOS_FRAMEBUFFER_ISOLATION_PROBE)
#define ROOT_FAULT_PROBE 1
#define ROOT_PROBE_NATIVE 4
#define ROOT_PROBE_CLIENT AOS_FB_PROBE_CLIENT
#define ROOT_PROBE_BADGE AOS_FB_PROBE_BADGE
#define ROOT_PROBE_ADDRESS AOS_FB_PROBE_ADDRESS
#define ROOT_PROBE_WRITE AOS_FB_PROBE_WRITE
#define ROOT_PROBE_MESSAGE AOS_FB_PROBE_MESSAGE
#elif defined(AGENTOS_LOG_ISOLATION_PROBE)
#define ROOT_FAULT_PROBE 1
#define ROOT_PROBE_NATIVE 3
#define ROOT_PROBE_CLIENT 0u
#define ROOT_PROBE_BADGE AOS_LOG_PROBE_BADGE
#define ROOT_PROBE_ADDRESS AOS_LOG_PROBE_ADDRESS
#define ROOT_PROBE_WRITE AOS_LOG_PROBE_WRITE
#define ROOT_PROBE_MESSAGE AOS_LOG_PROBE_MESSAGE
#elif defined(AGENTOS_OPERATOR_ISOLATION_PROBE)
#define ROOT_FAULT_PROBE 1
#define ROOT_PROBE_NATIVE 3
#define ROOT_PROBE_CLIENT 0u
#define ROOT_PROBE_BADGE AOS_OPERATOR_PROBE_BADGE
#define ROOT_PROBE_ADDRESS AOS_OPERATOR_PROBE_ADDRESS
#define ROOT_PROBE_WRITE AOS_OPERATOR_PROBE_WRITE
#define ROOT_PROBE_MESSAGE AOS_OPERATOR_PROBE_MESSAGE
#elif defined(AGENTOS_INSPECT_WRITE_PROBE)
#define ROOT_FAULT_PROBE 1
#define ROOT_PROBE_NATIVE 2
#define ROOT_PROBE_CLIENT 0u
#define ROOT_PROBE_BADGE 0xa0540001u
#define ROOT_PROBE_ADDRESS AOS_INSPECT_BOOT_VA
#define ROOT_PROBE_WRITE 1u
#define ROOT_PROBE_MESSAGE "[rt] inspect: expected read-only page write fault verified\n"
#elif defined(AGENTOS_NATIVE_NET_ISOLATION_PROBE)
#define ROOT_FAULT_PROBE 1
#define ROOT_PROBE_NATIVE 1
#define ROOT_PROBE_CLIENT 2u
#define ROOT_PROBE_BADGE AOS_NATIVE_NET_PROBE_BADGE
#define ROOT_PROBE_ADDRESS AOS_NATIVE_NET_PROBE_ADDRESS
#define ROOT_PROBE_WRITE AOS_NATIVE_NET_PROBE_WRITE
#define ROOT_PROBE_MESSAGE AOS_NATIVE_NET_PROBE_MESSAGE
#elif defined(AOS_VMM_ISOLATION_PROBE)
#define ROOT_FAULT_PROBE 1
#define ROOT_PROBE_NATIVE 0
#define ROOT_PROBE_CLIENT AOS_VMM_PROBE_CLIENT
#define ROOT_PROBE_BADGE AOS_VMM_PROBE_BADGE
#define ROOT_PROBE_ADDRESS AOS_VMM_PROBE_ADDRESS
#define ROOT_PROBE_WRITE AOS_VMM_PROBE_WRITE
#define ROOT_PROBE_MESSAGE AOS_VMM_PROBE_MESSAGE
#endif
#ifdef ROOT_FAULT_PROBE
#include "serial_log.h"
#endif
#include <platform/net_host_layout.h> /* host net MMIO/private DMA/shared bridge */
#include <platform/guest_memory_layout.h> /* guest GPA and VMM HVA windows        */
#include "pd_startup_record.h" /* pd_startup_record_t, PD_STARTUP_RECORD_VA      */
#include <platform/inspect.h>
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
 */
#if defined(BOARD_qemu_virt_aarch64)
extern const system_desc_t system_desc_aarch64;
#define SYSTEM_DESC (&system_desc_aarch64)
#elif defined(__x86_64__)
extern const system_desc_t system_desc_x86_64;
#define SYSTEM_DESC (&system_desc_x86_64)
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

/* VMM-private caps installed into profile-backed VMM CSpaces.
 *
 * These slots deliberately match the fixed libvmm registration offsets used
 * by the VMM PDs.  They are well above the low service/IRQ slots and fit in
 * the VMM PDs' 1024-slot CNodes.
 */
#define VMM_GUEST_TCB_SLOT_BASE   AOS_GUEST_TCB_CAP_BASE
#define VMM_GUEST_VCPU_SLOT_BASE  AOS_GUEST_VCPU_CAP_BASE
#define VMM_FAULT_BADGE_BASE      (1ULL << 62)
/* Guest TCB IPC buffer: next 4K after the debug UART page. Must not share
 * the VMM thread's buffer — seL4 forbids two TCBs on one IPC page, and a
 * guest VMFault would clobber in-flight VMM syscalls. L1 for
 * [0x10000000, 0x11FFFFF] is already installed with the VMM IPC mapping. */
#define VMM_GUEST_IPC_BUF_VA      0x0000000010002000UL

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
/*
 * Each VMM gets one sched context for the VMM PD and one for the guest vCPU.
 * A 90% budget works for a single guest but overcommits the single-core QEMU
 * E2E path when both configured guests run together. Keep the pair small enough
 * that two guests can make forward progress concurrently.
 */
#define VMM_SC_BUDGET_US          25000u
#define VMM_SC_PERIOD_US          100000u
/*
 * Guest fault senders share the VMM endpoint with vm_manager control calls.
 * Keep the relay path monotonic above guests: cc_pd 164, vibe_engine 165,
 * vm_manager 170, then the VMM PD at 250.  This prevents active device
 * services at priority 160 from starving a downstream lifecycle call.
 * An always-faulting guest therefore cannot starve CONSOLE_DRAIN, SUSPEND,
 * or DESTROY requests indefinitely.
 */
#define VMM_GUEST_PRIORITY        150u

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

#define AOS_MAX_GUEST_RAM_REGIONS 4u
#define AOS_MAX_GUEST_LARGE_FRAMES 1024u

typedef struct guest_ram_reservation {
    uint32_t pd_index;
    uint8_t mr_index;
    uint16_t frame_count;
    uint16_t first_frame;
} guest_ram_reservation_t;

static seL4_CPtr g_guest_large_frames[AOS_MAX_GUEST_LARGE_FRAMES];
static seL4_CPtr g_guest_large_frame_aliases[AOS_MAX_GUEST_LARGE_FRAMES];
static guest_ram_reservation_t
    g_guest_ram_reservations[AOS_MAX_GUEST_RAM_REGIONS];
static uint32_t g_guest_ram_reservation_count;

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
 * pd->irqs[], the root task calls seL4_IRQControl_Get, binds the handler to
 * the PD notification, and moves the cap into the PD's CNode at slot
 * (PD_IRQHANDLER_SLOT_BASE + i).  No root handler-cap alias is retained.
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

        if (notification_cap != seL4_CapNull) {
            seL4_Word badged_ntfn_slot = ut_alloc_slot();
            if (badged_ntfn_slot == seL4_CapNull) {
                dbg_puts("[rt] WARN: no slot for badged IRQ notification\n");
                (void)seL4_CNode_Delete(seL4_CapInitThreadCNode,
                                        root_irq_slot, 64u);
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
                (void)seL4_CNode_Delete(seL4_CapInitThreadCNode,
                                        root_irq_slot, 64u);
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
                (void)seL4_CNode_Delete(seL4_CapInitThreadCNode,
                                        root_irq_slot, 64u);
                continue;
            }
        }

        /*
         * Transfer rather than copy the handler capability.  IRQControl is
         * retained by the root task as initial authority, but the resulting
         * IRQ handler cap exists only in the designated driver/VMM CSpace.
         */
        err = seL4_CNode_Move(pd_cnode,
                              dest_slot,
                              (uint8_t)pd_cnode_depth,
                              seL4_CapInitThreadCNode,
                              root_irq_slot,
                              64u);
        if (err != seL4_NoError) {
            dbg_puts("[rt] WARN: IRQ cap move failed irq=");
            dbg_hex((seL4_Word)d->irq_number);
            dbg_puts(" err=");
            dbg_hex((seL4_Word)err);
            dbg_puts("\n");
            (void)seL4_CNode_Delete(seL4_CapInitThreadCNode,
                                    root_irq_slot, 64u);
            continue;
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
#define AGENTOS_UART_VA  0x10001000UL  /* root bootstrap, then serial_pd driver VA   */

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
 * Slots are 0x200-byte windows inside the 4 KB page at 0x0A000000. Guest
 * VMMs never map this host page; cc_pd maps a copy at its private VA to drive
 * slot 2 for the host relay.
 *
 * Guest IPA 0x0A010000 (emulated virtio-net) is outside this page on purpose:
 * it must remain unmapped so accesses fault into guest_vmm.
 */
#define VIRTIO_MMIO_PAGE_PA  0x0A000000UL

/* VirtIO serial device for cc_pd ↔ host socket bridge.
 * QEMU flags: -device virtio-serial-device,bus=virtio-mmio-bus.2,id=vser0
 *             -device virtconsole,bus=vser0.0,chardev=cc_pd_char,name=cc.0
 * virtio-mmio-bus.2 = PA 0x0A000400, inside the first virtio-mmio page (PA 0x0A000000). */
#define SERIAL_SHMEM_VA       0x10005000UL  /* MSG_SERIAL_* transfer page in client PDs   */
#define RT_VQ_SCRATCH_VA      0x60000000UL  /* Root-task scratch VA to write startup PAs */
#define RT_BLK_SCRATCH_VA     0xA0000000UL  /* Above max embedded live-media PD bundle  */

/* UART starts in the root CSpace for bounded bootstrap output, then is moved
 * into serial_pd.  No root or non-driver copy survives that handoff. */
static seL4_CPtr g_uart_frame_cap = seL4_CapNull;
static seL4_CPtr g_serial_shmem_frame_cap = seL4_CapNull;
static seL4_CPtr g_virtio_mmio_frame_cap = seL4_CapNull;
static seL4_CPtr g_host_blk_mmio_frame_cap = seL4_CapNull;
static seL4_CPtr g_blk_shared_frame_cap = seL4_CapNull;
/* Shared sDDF block region: guest request/response queues and data cells,
 * mapped wholly into blk_virt, with one client frame mapped into each VMM. */
static seL4_CPtr g_blk_virt_frame_caps[AOS_BLK_SHMEM_FRAMES];
static seL4_CPtr g_serial_virt_frames[AOS_SERIAL_FRAMES];
static seL4_CPtr g_pd_notifications[SYSTEM_MAX_PDS];
#if defined(__x86_64__) && defined(AGENTOS_X86_VTX)
static seL4_CPtr g_x86_vtx_proof_endpoint = seL4_CapNull;
#endif
#ifdef AGENTOS_LOG_RINGS
static seL4_CPtr g_log_frames[AOS_LOG_CLIENTS];
static aos_log_config_t g_log_config;

static seL4_Error log_map_copy(seL4_CPtr vspace, seL4_CPtr frame,
                              seL4_Word va, int writable)
{
    seL4_CPtr copy = ut_alloc_slot();
    if (!copy) return seL4_NotEnoughMemory;
    seL4_Error err = seL4_CNode_Copy(seL4_CapInitThreadCNode, copy, 64u,
        seL4_CapInitThreadCNode, frame, 64u, seL4_CapRights_new(0, 0, 1, writable));
    if (err) return err;
    return pd_vspace_map_device_frame(vspace, copy, va);
}

static seL4_Error provision_log_config(const pd_desc_t *pd, uint32_t index,
                                       seL4_CPtr vspace, seL4_CPtr cnode,
                                       uint32_t drain_index)
{
    uint32_t role = pd->self_svc_id == SVC_ID_LOG_DRAIN ? AOS_LOG_SERVER :
        g_log_config.clients[index].enabled ? AOS_LOG_CLIENT : AOS_LOG_DISABLED;
    seL4_Error err;
    if (role == AOS_LOG_CLIENT) {
        err = log_map_copy(vspace, g_log_frames[index], AOS_LOG_CLIENT_VA, 1);
        if (err) return err;
        err = seL4_CNode_Mint(cnode, AOS_LOG_NOTIFY_CAP, pd->cnode_size_bits,
            seL4_CapInitThreadCNode, g_pd_notifications[drain_index], 64u,
            seL4_CapRights_new(0, 0, 0, 1), AOS_LOG_WAKE);
        if (err) return err;
    } else if (role == AOS_LOG_SERVER) {
        for (uint32_t i = 0; i < g_log_config.count; i++) {
            if (!g_log_frames[i]) continue;
            err = log_map_copy(vspace, g_log_frames[i],
                AOS_LOG_SERVER_VA + i * AOS_LOG_PAGE, 1);
            if (err) return err;
        }
    }
    seL4_CPtr frame = seL4_CapNull;
    err = ut_alloc_cap(seL4_ARM_SmallPageObject, 0u, &frame);
    if (err) return err;
    err = pd_vspace_map_device_frame(seL4_CapInitThreadVSpace, frame, RT_VQ_SCRATCH_VA);
    if (err) return err;
    aos_log_config_t *config = (void *)RT_VQ_SCRATCH_VA;
    *config = g_log_config;
    config->role = role;
    config->slot = index;
    AGENTOS_MEMORY_FENCE();
    seL4_ARCH_Page_Unmap(frame);
    return log_map_copy(vspace, frame, AOS_LOG_CONFIG_VA, 0);
}
#endif
static seL4_CPtr g_host_net_mmio_frame_cap = seL4_CapNull;
static seL4_CPtr g_net_shared_frame_caps[AOS_NET_SHMEM_FRAMES];
static seL4_CPtr g_net_dma_frame_cap = seL4_CapNull;
static seL4_CPtr g_host_secondary_blk_mmio_frame_cap = seL4_CapNull;
static seL4_CPtr g_gic_vcpu_frame_cap = seL4_CapNull;

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

static int pd_is_guest_vmm(const pd_desc_t *pd)
{
    return pd->self_svc_id == SVC_ID_GUEST_VMM_PRIMARY ||
           pd->self_svc_id == SVC_ID_GUEST_VMM_SECONDARY;
}

static int pd_is_secondary_guest_vmm(const pd_desc_t *pd)
{
    return pd->self_svc_id == SVC_ID_GUEST_VMM_SECONDARY;
}

#if defined(__aarch64__)
static seL4_Error reserve_guest_ram_frames(const system_desc_t *sys)
{
    const size_t large_page = (size_t)1u << seL4_ARCH_LargePageBits;
    uint32_t next_frame = 0u;

    g_guest_ram_reservation_count = 0u;
    for (uint32_t i = 0u; i < sys->pd_count; i++) {
        const pd_desc_t *pd = &sys->pds[i];
        if (!pd_is_guest_vmm(pd)) {
            continue;
        }
        for (uint8_t j = 0u; j < pd->mr_count; j++) {
            const memory_region_desc_t *mr = &pd->memory_regions[j];
            if (!name_eq(mr->name, "guest_ram")) continue;
            if ((mr->size & (large_page - 1u)) != 0u ||
                g_guest_ram_reservation_count >=
                    AOS_MAX_GUEST_RAM_REGIONS) {
                return seL4_InvalidArgument;
            }
            uint32_t count = (uint32_t)(mr->size / large_page);
            if (count == 0u ||
                count > AOS_MAX_GUEST_LARGE_FRAMES - next_frame) {
                return seL4_NotEnoughMemory;
            }

            guest_ram_reservation_t *reservation =
                &g_guest_ram_reservations[g_guest_ram_reservation_count];
            reservation->pd_index = i;
            reservation->mr_index = j;
            reservation->first_frame = (uint16_t)next_frame;
            reservation->frame_count = (uint16_t)count;
            for (uint32_t frame = 0u; frame < count; frame++) {
                seL4_Error err = ut_alloc_cap(
                    (uint32_t)seL4_ARCH_LargePageObject, 0u,
                    &g_guest_large_frames[next_frame]);
                if (err != seL4_NoError) return err;
                next_frame++;
            }
            g_guest_ram_reservation_count++;
        }
    }
    return seL4_NoError;
}

static const guest_ram_reservation_t *
guest_ram_reservation_for(uint32_t pd_index, uint8_t mr_index)
{
    for (uint32_t i = 0u; i < g_guest_ram_reservation_count; i++) {
        const guest_ram_reservation_t *reservation =
            &g_guest_ram_reservations[i];
        if (reservation->pd_index == pd_index &&
            reservation->mr_index == mr_index) {
            return reservation;
        }
    }
    return NULL;
}

static seL4_Error map_guest_ram_reservation(
    const guest_ram_reservation_t *reservation,
    seL4_CPtr vmm_vspace, seL4_CPtr guest_vspace,
    seL4_Word hva_base, seL4_Word gpa_base, int writable)
{
    seL4_Error err = pd_vspace_map_reserved_region(
        guest_vspace, gpa_base,
        &g_guest_large_frames[reservation->first_frame],
        reservation->frame_count, writable);
    if (err != seL4_NoError) return err;

    for (uint32_t i = 0u; i < reservation->frame_count; i++) {
        seL4_Word alias_slot = ut_alloc_slot();
        if (alias_slot == seL4_CapNull) return seL4_NotEnoughMemory;
        err = seL4_CNode_Copy(
            seL4_CapInitThreadCNode, alias_slot, 64u,
            seL4_CapInitThreadCNode,
            g_guest_large_frames[reservation->first_frame + i], 64u,
            seL4_AllRights);
        if (err != seL4_NoError) return err;
        g_guest_large_frame_aliases[reservation->first_frame + i] =
            (seL4_CPtr)alias_slot;
    }
    return pd_vspace_map_reserved_region(
        vmm_vspace, hva_base,
        &g_guest_large_frame_aliases[reservation->first_frame],
        reservation->frame_count, writable);
}
#endif

/* True iff name starts with prefix. */
static int name_has_prefix(const char *name, const char *prefix)
{
    while (*prefix) {
        if (*name != *prefix) return 0;
        name++; prefix++;
    }
    return 1;
}

/*
 * pd_is_parameterized — true for PDs that consume a per-instance startup
 * record (agentos-3ev): swap_slot[_N], app_slot[_N], wg_net, vibe_swap.
 */
static int pd_is_parameterized(const char *name)
{
    return name_has_prefix(name, "swap_slot") ||
           name_has_prefix(name, "app_slot")  ||
           name_eq(name, "wg_net")            ||
           name_eq(name, "vibe_swap");
}

/*
 * pd_startup_slot_id — derive the per-instance slot id from a PD name of the
 * form "<base>_<n>" (e.g. swap_slot_2 → 2).  Names without a numeric suffix
 * (e.g. "wg_net", "swap_slot") map to slot 0.
 */
static uint32_t pd_startup_slot_id(const char *name)
{
    const char *last_us = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '_') last_us = p;
    }
    if (!last_us || last_us[1] == '\0') return 0u;
    uint32_t v = 0u;
    for (const char *d = last_us + 1; *d; d++) {
        if (*d < '0' || *d > '9') return 0u;  /* non-numeric suffix → slot 0 */
        v = v * 10u + (uint32_t)(*d - '0');
    }
    return v;
}

/*
 * pd_fill_startup_record — populate *rec for a parameterized PD.
 *
 * Slot id comes from the PD name suffix.  The controller-notification cap and
 * peer endpoint caps are taken from the CNode slots the root task already
 * provisions for this PD via pd->init_eps[] (see PD_CNODE_SLOT_* in
 * system_desc.h).  This replaces the legacy hard-coded slot 0 / NULL cap.
 *
 * Until these PDs are listed in the system descriptor with concrete init_eps,
 * the cap fields resolve to PD_STARTUP_CAP_NONE, which the wrappers treat
 * exactly like the old default — but the slot id is now always correct.
 */
static void pd_fill_startup_record(const pd_desc_t *pd, pd_startup_record_t *rec)
{
    pd_startup_record_init(rec);
    rec->slot_id = pd_startup_slot_id(pd->name);

    /*
     * Translate the PD's already-provisioned init_eps into the record's cap
     * fields, expressed as CNode slot numbers (valid as seL4_CPtr in the PD's
     * own cap space):
     *   - an init_ep destined for the cap-broker slot is surfaced as the
     *     controller-notification cap (the broker is the controller path);
     *   - every non-standard init_ep (slot >= PD_CNODE_SLOT_VIBE_ENGINE_EP) is
     *     surfaced as a generic peer ep, so vibe_swap's four worker eps and
     *     app_slot's spawn-server ep become visible to the wrapper.
     * Standard reserved slots (nameserver, serial, log_drain, self, …) are not
     * per-instance peers and are intentionally skipped.
     */
    uint32_t peer_n = 0u;
    for (uint32_t e = 0u; e < pd->init_ep_count && e < PD_MAX_INIT_EPS; e++) {
        uint16_t slot = pd->init_eps[e].cnode_slot;
        if (slot == PD_CNODE_SLOT_CAP_BROKER_EP) {
            rec->controller_ntfn = (uint32_t)slot;
        } else if (slot >= PD_CNODE_SLOT_VIBE_ENGINE_EP &&
                   peer_n < PD_STARTUP_MAX_PEER_EPS) {
            rec->peer_ep[peer_n++] = (uint32_t)slot;
        }
    }
    rec->peer_ep_count = peer_n;
}

/*
 * provision_pd_startup_record — allocate a frame, write the populated record
 * via a root-task scratch mapping, then remap it read-into the PD's VSpace at
 * PD_STARTUP_RECORD_VA.  Mirrors the cc_pd startup-record mechanism.
 *
 * Returns seL4_NoError on success.  A single frame cap can only be mapped
 * once, so we unmap from the root task before mapping into the PD.
 */
static seL4_Error provision_pd_startup_record(const pd_desc_t *pd,
                                              seL4_CPtr pd_vspace)
{
    seL4_CPtr frame = seL4_CapNull;
    seL4_Error ve = ut_alloc_cap(seL4_ARM_SmallPageObject, 0u, &frame);
    if (ve != seL4_NoError) return ve;

    ve = pd_vspace_map_device_frame(seL4_CapInitThreadVSpace, frame,
                                    RT_VQ_SCRATCH_VA);
    if (ve != seL4_NoError) return ve;

    pd_startup_record_t *rec = (pd_startup_record_t *)RT_VQ_SCRATCH_VA;
    pd_fill_startup_record(pd, rec);
    AGENTOS_MEMORY_FENCE();

    seL4_ARCH_Page_Unmap(frame);
    return pd_vspace_map_device_frame(pd_vspace, frame, PD_STARTUP_RECORD_VA);
}

#ifdef CONFIG_KERNEL_MCS
static seL4_Word sched_node_for_pd(const pd_desc_t *pd)
{
    if (pd_is_secondary_guest_vmm(pd)) {
        return 1u;
    }
    return 0u;
}

static seL4_CPtr schedcontrol_for_node(const seL4_BootInfo *bi, seL4_Word node)
{
    if (node >= bi->numNodes) {
        node = 0u;
    }
    if (bi->schedcontrol.start + node >= bi->schedcontrol.end) {
        node = 0u;
    }
    return (seL4_CPtr)(bi->schedcontrol.start + node);
}
#endif

#if defined(__aarch64__)
static seL4_Error setup_vmm_guest_vcpu(const pd_desc_t *pd,
                                        uint32_t         pd_index,
                                        seL4_CPtr        pd_cnode,
                                        seL4_CPtr        guest_vspace,
                                        seL4_CPtr        ipc_buf_cap,
                                        seL4_Word        ipc_buf_va,
                                        seL4_CPtr        self_ep,
                                        const seL4_BootInfo *bi)
{
    if (!pd_is_guest_vmm(pd)) {
        return seL4_NoError;
    }

    if (self_ep == seL4_CapNull) {
        dbg_puts("[rt] VMM guest setup skipped: missing self endpoint\n");
        return seL4_InvalidCapability;
    }

    seL4_Word guest_tcb_slot = ut_alloc_slot();
    seL4_Word guest_vcpu_slot = ut_alloc_slot();
    if (guest_tcb_slot == seL4_CapNull ||
        guest_vcpu_slot == seL4_CapNull) {
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

    seL4_CPtr guest_ipc_cap = seL4_CapNull;
    err = ut_alloc_cap(seL4_ARM_SmallPageObject, 0u, &guest_ipc_cap);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest IPC frame alloc err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }
    err = pd_vspace_map_device_frame(guest_vspace, guest_ipc_cap,
                                     VMM_GUEST_IPC_BUF_VA);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest IPC map err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }
    (void)ipc_buf_va;
    (void)ipc_buf_cap;

    seL4_Word cspace_root_data =
        (seL4_Word)(seL4_WordBits - (uint32_t)pd->cnode_size_bits);
    err = seL4_TCB_Configure((seL4_CPtr)guest_tcb_slot,
                              pd_cnode,
                              cspace_root_data,
                              guest_vspace,
                              0u,
                              VMM_GUEST_IPC_BUF_VA,
                              guest_ipc_cap);
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
              schedcontrol_for_node(bi, sched_node_for_pd(pd)),
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

    err = seL4_TCB_SetSchedParams((seL4_CPtr)guest_tcb_slot,
                                  seL4_CapInitThreadTCB,
                                  255u,
                                  VMM_GUEST_PRIORITY,
                                  (seL4_CPtr)guest_sc_slot,
                                  /* Guest faults must land on the VMM listen EP.
                                   * PD TCBs use a raw endpoint cap here (g_fault_ep).
                                   * A badged mint was accepted but VCPUFaults still
                                   * arrived on the root-task fault EP instead. */
                                  self_ep);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest SetSchedParams err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }
#else
    err = seL4_TCB_SetPriority((seL4_CPtr)guest_tcb_slot,
                               seL4_CapInitThreadTCB,
                               VMM_GUEST_PRIORITY);
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
    /* The VMM pauses execution by detaching this SC from its guest TCB.
     * Unlike TCB_Suspend, that preserves queued guest fault IPC. */
    err = seL4_CNode_Copy(pd_cnode,
                          AOS_GUEST_SC_CAP_BASE,
                          (uint8_t)pd->cnode_size_bits,
                          seL4_CapInitThreadCNode,
                          guest_sc_slot,
                          64u,
                          seL4_AllRights);
    if (err != seL4_NoError) {
        dbg_puts("[rt] VMM guest SC copy err=");
        dbg_hex((seL4_Word)err);
        dbg_puts("\n");
        return err;
    }
    cap_acct_record(seL4_CapNull, (seL4_CPtr)guest_sc_slot,
                    seL4_SchedContextObject, pd_index, pd->name);
#endif

    dbg_puts("[rt] VMM guest caps installed tcb=");
    dbg_hex((seL4_Word)guest_tcb_slot);
    dbg_puts(" vcpu=");
    dbg_hex((seL4_Word)guest_vcpu_slot);
    dbg_puts(" ipc_va=");
    dbg_hex((seL4_Word)VMM_GUEST_IPC_BUF_VA);
    dbg_puts("\n");
    return seL4_NoError;
}
#endif

#if defined(__x86_64__) && defined(AGENTOS_X86_VTX)
/*
 * Provision the deliberately small VMX/EPT qualification guest.  On x86 a
 * SysVMEnter call runs the VCPU bound to the calling VMM TCB, unlike the
 * AArch64 guest-TCB model above. The first frame contains HLT at GPA 0x1000;
 * four further frames form its long-mode guest page-table walk. Their EPT
 * mappings have no host-device capability or guest I/O authority.
 */
#ifdef AGENTOS_X86_FIRMWARE_RESET
extern const uint8_t _binary_x86_firmware_bin_start[];
extern const uint8_t _binary_x86_firmware_bin_end[];

static seL4_Error setup_x86_firmware(const pd_desc_t *pd, uint32_t pd_index,
                                    seL4_CPtr pd_cnode, seL4_CPtr vmm_tcb,
                                    seL4_CPtr vmm_vspace)
{
    if (!pd_is_guest_vmm(pd) || pd->self_svc_id != SVC_ID_GUEST_VMM_PRIMARY ||
        pd->cnode_size_bits < 10u ||
        (uintptr_t)_binary_x86_firmware_bin_end -
        (uintptr_t)_binary_x86_firmware_bin_start != AOS_X86_FIRMWARE_BYTES) {
        return seL4_InvalidArgument;
    }
    seL4_CPtr objects[5] = {0};
    const uint32_t types[5] = {
        seL4_X86_VCPUObject, seL4_X86_EPTPML4Object,
        seL4_X86_EPTPDPTObject, seL4_X86_EPTPDObject, seL4_X86_EPTPDObject,
    };
    seL4_Error err;
    for (unsigned i = 0u; i < 5u; i++) {
        err = ut_alloc_cap(types[i], 0u, &objects[i]);
        if (err != seL4_NoError) return err;
        (void)cap_acct_record(seL4_CapNull, objects[i], types[i], pd_index, pd->name);
    }
    const seL4_Word attr = seL4_X86_EPT_Default_VMAttributes;
    err = seL4_X86_ASIDPool_Assign(seL4_CapInitThreadASIDPool, objects[1]);
    if (err != seL4_NoError) return err;
    err = seL4_X86_EPTPDPT_Map(objects[2], objects[1], 0u, attr);
    if (err != seL4_NoError) return err;
    err = seL4_X86_EPTPD_Map(objects[3], objects[1], 0u, attr);
    if (err != seL4_NoError) return err;
    err = seL4_X86_EPTPD_Map(objects[4], objects[1], 0xc0000000u, attr);
    if (err != seL4_NoError) return err;

    const seL4_Word page_bytes = 1u << 21;
    /* Root initializes private guest frames through one temporary mapping.
     * ROM has no guest write permission. No host device or MMIO is mapped. */
    for (unsigned i = 0u; i < (AOS_X86_FIRMWARE_RAM + AOS_X86_FIRMWARE_BYTES) / page_bytes; i++) {
        seL4_CPtr frame;
        const seL4_Word offset = (seL4_Word)i * page_bytes;
        const int rom = offset >= AOS_X86_FIRMWARE_RAM;
        const seL4_Word rom_offset = rom ? offset - AOS_X86_FIRMWARE_RAM : 0u;
        const seL4_Word gpa = rom ? AOS_X86_FIRMWARE_BASE + rom_offset : offset;
        err = ut_alloc_cap(seL4_X86_LargePageObject, 0u, &frame);
        if (err != seL4_NoError) return err;
        (void)cap_acct_record(seL4_CapNull, frame, seL4_X86_LargePageObject, pd_index, pd->name);
        err = pd_vspace_map_device_frame(seL4_CapInitThreadVSpace, frame, 0x70000000u);
        if (err != seL4_NoError) return err;
        volatile uint8_t *dst = (volatile uint8_t *)0x70000000u;
        for (seL4_Word n = 0u; n < page_bytes; n++) {
            dst[n] = rom ? _binary_x86_firmware_bin_start[rom_offset + n] : 0u;
        }
        AGENTOS_MEMORY_FENCE();
        err = seL4_X86_Page_Unmap(frame);
        if (err != seL4_NoError) return err;
        /* VMM owns private guest RAM for emulated I/O and may inspect ROM.
         * ROM stays read-only. It gets no host-device mapping. */
        seL4_CPtr copy = ut_alloc_slot();
        if (copy == seL4_CapNull) return seL4_NotEnoughMemory;
        err = seL4_CNode_Copy(seL4_CapInitThreadCNode, copy, 64u,
                              seL4_CapInitThreadCNode, frame, 64u,
                              seL4_CapRights_new(0u, 0u, 1u, !rom));
        if (err != seL4_NoError) return err;
        (void)cap_acct_record(frame, copy, seL4_X86_LargePageObject, pd_index, pd->name);
        err = pd_vspace_map_device_frame(vmm_vspace, copy,
                rom ? AOS_X86_FIRMWARE_ROM_VA + rom_offset : AOS_X86_FIRMWARE_RAM_VA + offset);
        if (err != seL4_NoError) return err;
        err = seL4_X86_Page_MapEPT(frame, objects[1], gpa,
                                   rom ? seL4_CapRights_new(0u, 0u, 1u, 0u) : seL4_AllRights, attr);
        if (err != seL4_NoError) return err;
    }
    err = seL4_X86_VCPU_SetTCB(objects[0], vmm_tcb);
    if (err != seL4_NoError) return err;
    err = seL4_TCB_SetEPTRoot(vmm_tcb, objects[1]);
    if (err != seL4_NoError) return err;
    err = seL4_CNode_Copy(pd_cnode, AOS_GUEST_VCPU_CAP_BASE,
                          (uint8_t)pd->cnode_size_bits, seL4_CapInitThreadCNode,
                          objects[0], 64u, seL4_AllRights);
    if (err != seL4_NoError) return err;
    dbg_puts("[rt] x86 VMX EPT proof provisioned\n");
    dbg_puts("[rt] x86 OVMF private RAM and read-only ROM provisioned\n");
    return seL4_NoError;
}
#endif

static seL4_Error setup_x86_vtx_proof(const pd_desc_t *pd, uint32_t pd_index,
                                      seL4_CPtr pd_cnode, seL4_CPtr vmm_tcb,
                                      seL4_CPtr vmm_vspace)
{
#ifdef AGENTOS_X86_FIRMWARE_RESET
    return setup_x86_firmware(pd, pd_index, pd_cnode, vmm_tcb, vmm_vspace);
#endif
    seL4_CPtr vcpu = seL4_CapNull;
    seL4_CPtr ept_pml4 = seL4_CapNull;
    seL4_CPtr ept_pdpt = seL4_CapNull;
    seL4_CPtr ept_pd = seL4_CapNull;
    seL4_CPtr ept_pt = seL4_CapNull;
    seL4_CPtr guest_page = seL4_CapNull;
    seL4_CPtr guest_pml4 = seL4_CapNull;
    seL4_CPtr guest_pdpt = seL4_CapNull;
    seL4_CPtr guest_pd = seL4_CapNull;
    seL4_CPtr guest_pt = seL4_CapNull;
    /* The EPT wire value is unchanged; SDK 2.3 corrects the syscall's enum
     * type from ordinary VM attributes to EPT attributes. */
    const seL4_Word ept_attr = seL4_X86_EPT_Default_VMAttributes;

    if (!pd_is_guest_vmm(pd) || pd->self_svc_id != SVC_ID_GUEST_VMM_PRIMARY) {
        return seL4_InvalidArgument;
    }
    if (pd->cnode_size_bits < 10u) {
        return seL4_InvalidArgument;
    }

    seL4_Error err = ut_alloc_cap(seL4_X86_VCPUObject, 0u, &vcpu);
    if (err != seL4_NoError) return err;
    err = ut_alloc_cap(seL4_X86_EPTPML4Object, 0u, &ept_pml4);
    if (err != seL4_NoError) return err;
    err = ut_alloc_cap(seL4_X86_EPTPDPTObject, 0u, &ept_pdpt);
    if (err != seL4_NoError) return err;
    err = ut_alloc_cap(seL4_X86_EPTPDObject, 0u, &ept_pd);
    if (err != seL4_NoError) return err;
    err = ut_alloc_cap(seL4_X86_EPTPTObject, 0u, &ept_pt);
    if (err != seL4_NoError) return err;
    err = ut_alloc_cap(seL4_X86_4K, 0u, &guest_page);
    if (err != seL4_NoError) return err;
    err = ut_alloc_cap(seL4_X86_4K, 0u, &guest_pml4);
    if (err != seL4_NoError) return err;
    err = ut_alloc_cap(seL4_X86_4K, 0u, &guest_pdpt);
    if (err != seL4_NoError) return err;
    err = ut_alloc_cap(seL4_X86_4K, 0u, &guest_pd);
    if (err != seL4_NoError) return err;
    err = ut_alloc_cap(seL4_X86_4K, 0u, &guest_pt);
    if (err != seL4_NoError) return err;

    err = pd_vspace_map_device_frame(seL4_CapInitThreadVSpace, guest_page,
                                     0x60000000u);
    if (err != seL4_NoError) return err;
    volatile uint8_t *guest_code = (volatile uint8_t *)0x60000000u;
    for (uint32_t i = 0u; i < 4096u; i++) {
        guest_code[i] = 0u;
    }
    guest_code[0] = 0xf4u; /* HLT */
    AGENTOS_MEMORY_FENCE();
    err = seL4_X86_Page_Unmap((seL4_X86_Page)guest_page);
    if (err != seL4_NoError) return err;
    const struct {
        seL4_CPtr page;
        seL4_Word entry;
    } page_tables[] = {
        { guest_pml4, AOS_X86_VTX_GUEST_PDPT_GPA | 3u },
        { guest_pdpt, AOS_X86_VTX_GUEST_PD_GPA | 3u },
        { guest_pd, AOS_X86_VTX_GUEST_PT_GPA | 3u },
        { guest_pt, AOS_X86_VTX_GUEST_RIP | 3u },
    };
    for (uint32_t i = 0u; i < sizeof(page_tables) / sizeof(page_tables[0]); i++) {
        err = pd_vspace_map_device_frame(seL4_CapInitThreadVSpace,
                                         page_tables[i].page, 0x60000000u);
        if (err != seL4_NoError) return err;
        volatile seL4_Word *entries = (volatile seL4_Word *)0x60000000u;
        for (uint32_t j = 0u; j < 512u; j++) {
            entries[j] = 0u;
        }
        entries[i == 3u ? 1u : 0u] = page_tables[i].entry;
        AGENTOS_MEMORY_FENCE();
        err = seL4_X86_Page_Unmap((seL4_X86_Page)page_tables[i].page);
        if (err != seL4_NoError) return err;
    }

    err = seL4_X86_ASIDPool_Assign((seL4_X86_ASIDPool)seL4_CapInitThreadASIDPool,
                                   ept_pml4);
    if (err != seL4_NoError) return err;
    err = seL4_X86_EPTPDPT_Map(ept_pdpt, ept_pml4, 0u, ept_attr);
    if (err != seL4_NoError) return err;
    err = seL4_X86_EPTPD_Map(ept_pd, ept_pml4, 0u, ept_attr);
    if (err != seL4_NoError) return err;
    err = seL4_X86_EPTPT_Map(ept_pt, ept_pml4, 0u, ept_attr);
    if (err != seL4_NoError) return err;
    err = seL4_X86_Page_MapEPT(guest_page, ept_pml4, AOS_X86_VTX_GUEST_RIP,
                               seL4_AllRights, ept_attr);
    if (err != seL4_NoError) return err;
    err = seL4_X86_Page_MapEPT(guest_pml4, ept_pml4,
                               AOS_X86_VTX_GUEST_PML4_GPA,
                               seL4_AllRights, ept_attr);
    if (err != seL4_NoError) return err;
    err = seL4_X86_Page_MapEPT(guest_pdpt, ept_pml4,
                               AOS_X86_VTX_GUEST_PDPT_GPA,
                               seL4_AllRights, ept_attr);
    if (err != seL4_NoError) return err;
    err = seL4_X86_Page_MapEPT(guest_pd, ept_pml4, AOS_X86_VTX_GUEST_PD_GPA,
                               seL4_AllRights, ept_attr);
    if (err != seL4_NoError) return err;
    err = seL4_X86_Page_MapEPT(guest_pt, ept_pml4, AOS_X86_VTX_GUEST_PT_GPA,
                               seL4_AllRights, ept_attr);
    if (err != seL4_NoError) return err;

    err = seL4_X86_VCPU_SetTCB((seL4_X86_VCPU)vcpu, (seL4_TCB)vmm_tcb);
    if (err != seL4_NoError) return err;
    err = seL4_TCB_SetEPTRoot((seL4_TCB)vmm_tcb, ept_pml4);
    if (err != seL4_NoError) return err;
    err = seL4_CNode_Copy(pd_cnode, AOS_GUEST_VCPU_CAP_BASE,
                          (uint8_t)pd->cnode_size_bits,
                          seL4_CapInitThreadCNode, vcpu, 64u, seL4_AllRights);
    if (err != seL4_NoError) return err;

    (void)cap_acct_record(seL4_CapNull, vcpu, seL4_X86_VCPUObject,
                          pd_index, pd->name);
    (void)cap_acct_record(seL4_CapNull, ept_pml4, seL4_X86_EPTPML4Object,
                          pd_index, pd->name);
    (void)cap_acct_record(seL4_CapNull, ept_pdpt, seL4_X86_EPTPDPTObject,
                          pd_index, pd->name);
    (void)cap_acct_record(seL4_CapNull, ept_pd, seL4_X86_EPTPDObject,
                          pd_index, pd->name);
    (void)cap_acct_record(seL4_CapNull, ept_pt, seL4_X86_EPTPTObject,
                          pd_index, pd->name);
    (void)cap_acct_record(seL4_CapNull, guest_page, seL4_X86_4K,
                          pd_index, pd->name);
    (void)cap_acct_record(seL4_CapNull, guest_pml4, seL4_X86_4K,
                          pd_index, pd->name);
    (void)cap_acct_record(seL4_CapNull, guest_pdpt, seL4_X86_4K,
                          pd_index, pd->name);
    (void)cap_acct_record(seL4_CapNull, guest_pd, seL4_X86_4K,
                          pd_index, pd->name);
    (void)cap_acct_record(seL4_CapNull, guest_pt, seL4_X86_4K,
                          pd_index, pd->name);
    dbg_puts("[rt] x86 VMX EPT proof provisioned\n");
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
#ifdef CONFIG_KERNEL_MCS
    dbg_puts(" nodes=");
    dbg_hex(bi->numNodes);
    dbg_puts(" schedcontrol=");
    dbg_hex(bi->schedcontrol.start);
    dbg_puts("..");
    dbg_hex(bi->schedcontrol.end);
#endif
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
        seL4_Error serial_shmem_err = ut_alloc_cap(seL4_ARM_SmallPageObject,
                                                   0u,
                                                   &g_serial_shmem_frame_cap);
        dbg_puts("[rt] serial transfer frame cap err=");
        dbg_hex((seL4_Word)serial_shmem_err);
        dbg_puts("\n");
    }

    {
        seL4_Error gic_err = ut_alloc_device_cap(GIC_VCPU_IF_PA,
                                                 &g_gic_vcpu_frame_cap);
        dbg_puts("[rt] GIC vCPU frame cap err=");
        dbg_hex((seL4_Word)gic_err);
        dbg_puts(" cap=");
        dbg_hex((seL4_Word)g_gic_vcpu_frame_cap);
        dbg_puts("\n");
    }

    {
        seL4_Error virtio_err = ut_alloc_device_cap(VIRTIO_MMIO_PAGE_PA,
                                                    &g_virtio_mmio_frame_cap);
        dbg_puts("[rt] virtio-mmio frame cap err=");
        dbg_hex((seL4_Word)virtio_err);
        dbg_puts(" cap=");
        dbg_hex((seL4_Word)g_virtio_mmio_frame_cap);
        dbg_puts("\n");
    }

#if defined(__aarch64__)
    {
        seL4_Error blk_err =
            ut_alloc_device_cap(AGENTOS_HOST_BLK_MMIO_PA,
                                &g_host_blk_mmio_frame_cap);
        dbg_puts("[rt] host blk virtio-mmio frame cap err=");
        dbg_hex((seL4_Word)blk_err);
        dbg_puts(" cap=");
        dbg_hex((seL4_Word)g_host_blk_mmio_frame_cap);
        dbg_puts("\n");
        if (blk_err == seL4_NoError) {
            blk_err = pd_vspace_map_device_frame(
                seL4_CapInitThreadVSpace, g_host_blk_mmio_frame_cap,
                RT_VQ_SCRATCH_VA);
            if (blk_err == seL4_NoError) {
                volatile uint32_t *blk_regs =
                    (volatile uint32_t *)RT_VQ_SCRATCH_VA;
                dbg_puts("[rt] host blk magic=");
                dbg_hex((seL4_Word)blk_regs[0]);
                dbg_puts(" version=");
                dbg_hex((seL4_Word)blk_regs[1]);
                dbg_puts(" device=");
                dbg_hex((seL4_Word)blk_regs[2]);
                dbg_puts("\n");
                seL4_ARCH_Page_Unmap(g_host_blk_mmio_frame_cap);
            }
        }
    }

    {
        seL4_Error blk_err =
            ut_alloc_cap(seL4_ARM_LargePageObject, 0u,
                         &g_blk_shared_frame_cap);
        seL4_Word blk_shared_pa = 0u;
        if (blk_err == seL4_NoError) {
            seL4_ARCH_Page_GetAddress_t r =
                seL4_ARCH_Page_GetAddress(g_blk_shared_frame_cap);
            blk_shared_pa = r.paddr;
            blk_err = pd_vspace_map_device_frame(
                seL4_CapInitThreadVSpace, g_blk_shared_frame_cap,
                RT_BLK_SCRATCH_VA);
        }
        if (blk_err == seL4_NoError) {
            agentos_blk_shared_meta_t *meta =
                (agentos_blk_shared_meta_t *)RT_BLK_SCRATCH_VA;
            meta->magic = AGENTOS_BLK_SHARED_MAGIC;
            meta->version = 1u;
            meta->paddr = (uint64_t)blk_shared_pa;
            meta->size = AGENTOS_BLK_SHARED_SIZE;
            AGENTOS_MEMORY_FENCE();
            seL4_ARCH_Page_Unmap(g_blk_shared_frame_cap);
        }
        dbg_puts("[rt] blk shared frame pa=");
        dbg_hex(blk_shared_pa);
        dbg_puts(" err=");
        dbg_hex((seL4_Word)blk_err);
        dbg_puts("\n");
    }

    {
        /* Shared sDDF block region (AOS_BLK_SHMEM_VA): one large page per
         * AOS_BLK_SHMEM_FRAMES, zero-filled by retype, mapped below into the
         * guest VMMs and blk_virt.  A failed frame leaves the whole array
         * null so no PD gets a partial region. */
        seL4_Error blk_err = seL4_NoError;
        for (uint32_t f = 0u; f < AOS_BLK_SHMEM_FRAMES; f++) {
            blk_err = ut_alloc_cap(seL4_ARM_LargePageObject, 0u,
                                   &g_blk_virt_frame_caps[f]);
            if (blk_err != seL4_NoError) {
                for (uint32_t g = 0u; g < AOS_BLK_SHMEM_FRAMES; g++) {
                    g_blk_virt_frame_caps[g] = seL4_CapNull;
                }
                break;
            }
        }
        dbg_puts("[rt] blk_virt shared region frames=");
        dbg_hex((seL4_Word)AOS_BLK_SHMEM_FRAMES);
        dbg_puts(" err=");
        dbg_hex((seL4_Word)blk_err);
        dbg_puts("\n");
    }

    {
        seL4_Error net_err =
            ut_alloc_device_cap(AGENTOS_HOST_NET_MMIO_PA,
                                &g_host_net_mmio_frame_cap);
        dbg_puts("[rt] host net virtio-mmio bus16 frame cap err=");
        dbg_hex((seL4_Word)net_err);
        dbg_puts(" cap=");
        dbg_hex((seL4_Word)g_host_net_mmio_frame_cap);
        dbg_puts("\n");
    }

    {
        seL4_Error net_err = seL4_NoError;
        for (uint32_t f = 0u; f < AOS_NET_SHMEM_FRAMES; ++f) {
            net_err = ut_alloc_cap(seL4_ARM_LargePageObject, 0u,
                                   &g_net_shared_frame_caps[f]);
            if (net_err != seL4_NoError) {
                for (uint32_t n = 0u; n < AOS_NET_SHMEM_FRAMES; ++n)
                    g_net_shared_frame_caps[n] = seL4_CapNull;
                break;
            }
        }
        dbg_puts("[rt] net agentOS shared frame err=");
        dbg_hex((seL4_Word)net_err);
        dbg_puts("\n");
    }

    {
        seL4_Error net_err =
            ut_alloc_cap(seL4_ARM_LargePageObject, 0u,
                         &g_net_dma_frame_cap);
        seL4_Word net_dma_pa = 0u;
        if (net_err == seL4_NoError) {
            seL4_ARCH_Page_GetAddress_t r =
                seL4_ARCH_Page_GetAddress(g_net_dma_frame_cap);
            net_dma_pa = r.paddr;
            net_err = pd_vspace_map_device_frame(
                seL4_CapInitThreadVSpace, g_net_dma_frame_cap,
                RT_BLK_SCRATCH_VA);
        }
        if (net_err == seL4_NoError) {
            agentos_net_host_dma_meta_t *meta =
                (agentos_net_host_dma_meta_t *)RT_BLK_SCRATCH_VA;
            meta->magic = AGENTOS_NET_HOST_DMA_MAGIC;
            meta->version = AGENTOS_NET_HOST_DMA_VERSION;
            meta->paddr = (uint64_t)net_dma_pa;
            meta->size = AGENTOS_NET_HOST_DMA_SIZE;
            AGENTOS_MEMORY_FENCE();
            seL4_ARCH_Page_Unmap(g_net_dma_frame_cap);
        }
        dbg_puts("[rt] net private DMA frame pa=");
        dbg_hex(net_dma_pa);
        dbg_puts(" err=");
        dbg_hex((seL4_Word)net_err);
        dbg_puts("\n");
    }
#endif

    {
        seL4_Error v31_err =
            ut_alloc_device_cap(AGENTOS_HOST_SECONDARY_BLK_PAGE_PA,
                                &g_host_secondary_blk_mmio_frame_cap);
        dbg_puts("[rt] host secondary block page cap err=");
        dbg_hex((seL4_Word)v31_err);
        dbg_puts(" cap=");
        dbg_hex((seL4_Word)g_host_secondary_blk_mmio_frame_cap);
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

    const system_desc_t *sys = SYSTEM_DESC;
    static aos_inspect_view_t inspect_view;
    seL4_CPtr inspect_cc_vspace = seL4_CapNull;
    seL4_CPtr inspect_operator_vspace = seL4_CapNull;
    inspect_view.flags = AOS_INSPECT_FLAG_BOOT | AOS_INSPECT_FLAG_PARTIAL |
                         AOS_INSPECT_FLAG_USED_LOWER_BOUND;

#if defined(__aarch64__)
    /*
     * Reserve all guest RAM as 2 MiB frames before loading any PD ELF. Small
     * boot objects fragment untyped watermarks; allocating guest RAM inside
     * the late PD loop forced hundreds of thousands of 4 KiB retypes.
     */
    {
        seL4_Error reserve_err = reserve_guest_ram_frames(sys);
        dbg_puts("[rt] early guest RAM large-page reservation err=");
        dbg_hex((seL4_Word)reserve_err);
        dbg_puts("\n");
    }
#endif

    /* Allocate notifications before spawning: serial_virt needs send-only
     * capabilities to VMM notifications even when those VMMs spawn later. */
    uint32_t serial_virt_index = SYSTEM_MAX_PDS;
    uint32_t blk_virt_index = SYSTEM_MAX_PDS;
    uint32_t net_virt_index = SYSTEM_MAX_PDS;
    uint32_t native_net_index = SYSTEM_MAX_PDS;
    uint32_t log_drain_index = SYSTEM_MAX_PDS;
#ifdef AGENTOS_FRAMEBUFFER_TEST
    uint32_t fb_service = SYSTEM_MAX_PDS;
    uint32_t fb_clients[AOS_FB_CLIENTS] = { SYSTEM_MAX_PDS, SYSTEM_MAX_PDS };
#endif
    for (uint32_t i = 0; i < sys->pd_count; i++) {
        const pd_desc_t *pd = &sys->pds[i];
        if (pd->self_svc_id == SVC_ID_SERIAL_VIRT) serial_virt_index = i;
        if (pd->self_svc_id == SVC_ID_BLK_VIRT) blk_virt_index = i;
        if (pd->self_svc_id == SVC_ID_NET_VIRT) net_virt_index = i;
        if (pd->self_svc_id == SVC_ID_NATIVE_RUST_PROBE) native_net_index = i;
        if (pd->self_svc_id == SVC_ID_LOG_DRAIN) log_drain_index = i;
#ifdef AGENTOS_FRAMEBUFFER_TEST
        if (pd->self_svc_id == SVC_ID_FRAMEBUFFER_QUEUE) fb_service = i;
        if (pd->self_svc_id == SVC_ID_FRAMEBUFFER_TEST0) fb_clients[0] = i;
        if (pd->self_svc_id == SVC_ID_FRAMEBUFFER_TEST1) fb_clients[1] = i;
#endif
        if (pd->irq_count || pd_is_guest_vmm(pd) ||
#ifdef AGENTOS_FRAMEBUFFER_TEST
            pd->self_svc_id == SVC_ID_FRAMEBUFFER_QUEUE ||
            pd->self_svc_id == SVC_ID_FRAMEBUFFER_TEST0 ||
            pd->self_svc_id == SVC_ID_FRAMEBUFFER_TEST1 ||
#endif
            pd->self_svc_id == SVC_ID_OPERATOR_SESSION ||
            pd->self_svc_id == SVC_ID_LOG_DRAIN ||
            pd->self_svc_id == SVC_ID_NET_VIRT ||
            pd->self_svc_id == SVC_ID_NATIVE_RUST_PROBE ||
            pd->self_svc_id == SVC_ID_BLK_VIRT ||
            pd->self_svc_id == SVC_ID_SERIAL_VIRT) {
            seL4_Error err = ut_alloc(seL4_NotificationObject,
                seL4_NotificationBits, seL4_CapInitThreadCNode,
                PD_SLOT_NTFN(i), 64u);
            if (err != seL4_NoError) {
                dbg_puts("[rt] notification allocation failed; refusing partial boot\n");
                return;
            }
            g_pd_notifications[i] = (seL4_CPtr)PD_SLOT_NTFN(i);
        }
    }
#ifdef AGENTOS_FRAMEBUFFER_TEST
    if (fb_service == SYSTEM_MAX_PDS || fb_clients[0] == SYSTEM_MAX_PDS ||
        fb_clients[1] == SYSTEM_MAX_PDS) return;
    for (uint32_t f = 0; f < AOS_FB_ARENA_FRAMES; ++f) {
        if (ut_alloc_cap(seL4_ARM_LargePageObject, 0u,
                         &g_framebuffer_arena[f]) != seL4_NoError) {
            dbg_puts("[rt] framebuffer arena allocation failed; refusing boot\n");
            return;
        }
    }
    for (uint32_t f = 0; f < AOS_FB_CLIENTS; ++f) {
        if (ut_alloc_cap(seL4_ARM_LargePageObject, 0u,
                         &g_framebuffer_frames[f]) != seL4_NoError) {
            dbg_puts("[rt] framebuffer queue allocation failed; refusing boot\n");
            return;
        }
    }
#endif
#if defined(__aarch64__)
    if (serial_virt_index != SYSTEM_MAX_PDS) {
        for (uint32_t f = 0; f < AOS_SERIAL_FRAMES; f++) {
            if (ut_alloc_cap(seL4_ARM_LargePageObject, 0u,
                             &g_serial_virt_frames[f]) != seL4_NoError) {
                dbg_puts("[rt] serial queue allocation failed; refusing partial boot\n");
                return;
            }
        }
    }
#endif

#ifdef AGENTOS_LOG_RINGS
    if (log_drain_index == SYSTEM_MAX_PDS || sys->pd_count > AOS_LOG_CLIENTS) {
        dbg_puts("[rt] missing log drain or excessive clients; refusing boot\n");
        return;
    }
    g_log_config.magic = AOS_LOG_CONFIG_MAGIC;
    g_log_config.version = AOS_LOG_VERSION;
    g_log_config.count = sys->pd_count;
    for (uint32_t i = 0; i < sys->pd_count; i++) {
        const pd_desc_t *pd = &sys->pds[i];
        aos_log_identity_t *id = &g_log_config.clients[i];
        id->enabled = pd->self_svc_id != SVC_ID_LOG_DRAIN && pd->self_svc_id != SVC_ID_SERIAL;
        id->service_id = pd->self_svc_id;
        for (uint32_t n = 0; n + 1 < sizeof(id->name) && pd->name[n]; n++) id->name[n] = pd->name[n];
        if (!id->enabled) continue;
        if (ut_alloc_cap(seL4_ARM_SmallPageObject, 0u, &g_log_frames[i]) != seL4_NoError ||
            pd_vspace_map_device_frame(seL4_CapInitThreadVSpace, g_log_frames[i], RT_VQ_SCRATCH_VA) != seL4_NoError) {
            dbg_puts("[rt] log ring allocation failed; refusing boot\n");
            return;
        }
        aos_log_ring_t *ring = (void *)RT_VQ_SCRATCH_VA;
        ring->magic = AOS_LOG_MAGIC;
        ring->pd_id = i;
        AGENTOS_MEMORY_FENCE();
        seL4_ARCH_Page_Unmap(g_log_frames[i]);
    }
#endif

    /* ── Step 4: Load and start each PD ───────────────────────────────────── */
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
#if defined(__aarch64__)
        seL4_CPtr guest_vspace = seL4_CapNull;
        if (pd_is_guest_vmm(pd)) {
            pd_vspace_result_t guest_vr =
                pd_vspace_create(seL4_CapInitThreadCNode,
                                 seL4_CapInitThreadASIDPool);
            if (guest_vr.error != 0) {
                dbg_puts("[rt] guest vspace_create fail err=");
                dbg_hex((seL4_Word)guest_vr.error);
                dbg_puts("\n");
                continue;
            }
            guest_vspace = guest_vr.vspace_cap;
        }
#endif

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
            if (pd_is_guest_vmm(pd)) {
                sc_budget = VMM_SC_BUDGET_US;
                sc_period = VMM_SC_PERIOD_US;
            }

            sc_err = seL4_SchedControl_ConfigureFlags(
                         schedcontrol_for_node(bi, sched_node_for_pd(pd)),
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
            seL4_CPtr pd_fault_ep = g_fault_ep;
#ifdef ROOT_FAULT_PROBE
            if ((ROOT_PROBE_NATIVE == 4 && pd->self_svc_id == SVC_ID_FRAMEBUFFER_TEST0 + ROOT_PROBE_CLIENT) ||
                (ROOT_PROBE_NATIVE == 3 && pd->self_svc_id == SVC_ID_OPERATOR_SESSION) ||
                (ROOT_PROBE_NATIVE == 2 && pd->self_svc_id == SVC_ID_CC_PD) ||
                (ROOT_PROBE_NATIVE == 1 && pd->self_svc_id == SVC_ID_NATIVE_RUST_PROBE) ||
                (ROOT_PROBE_NATIVE == 0 && pd_is_guest_vmm(pd) &&
                 (uint32_t)pd_is_secondary_guest_vmm(pd) == ROOT_PROBE_CLIENT)) {
                pd_fault_ep = ut_alloc_slot();
                if (pd_fault_ep == seL4_CapNull ||
                    seL4_CNode_Mint(seL4_CapInitThreadCNode, pd_fault_ep, 64u,
                                   seL4_CapInitThreadCNode, g_fault_ep, 64u,
                                   seL4_AllRights, ROOT_PROBE_BADGE) != seL4_NoError) {
                    dbg_puts("[rt] block isolation probe endpoint failed\n");
                    continue;
                }
            }
#endif
            sc_err = seL4_TCB_SetSchedParams(
                         tr.tcb_cap,
                         seL4_CapInitThreadTCB, /* authority: root TCB, MCP=255 */
                         255u,                  /* mcp */
                         (seL4_Word)pd->priority,
                         (seL4_CPtr)PD_SLOT_SC(i),
                         pd_fault_ep);
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

        seL4_CPtr pd_ntfn_cap = g_pd_notifications[i];
        if (pd_ntfn_cap != seL4_CapNull) {
            seL4_Error ntfn_err = seL4_TCB_BindNotification(tr.tcb_cap, pd_ntfn_cap);
            if (ntfn_err != seL4_NoError) {
                dbg_puts("[rt] notification bind failed; refusing PD start\n");
                continue;
            }
        }

#ifdef AGENTOS_FRAMEBUFFER_TEST
        if (i == fb_service || i == fb_clients[0] || i == fb_clients[1]) {
            seL4_Error err = seL4_CNode_Copy(pd_cnode, PD_CNODE_SLOT_FB_WAIT,
                pd->cnode_size_bits, seL4_CapInitThreadCNode, g_pd_notifications[i],
                64u, seL4_CapRights_new(0, 0, 1, 0));
            for (uint32_t f = 0; f < AOS_FB_CLIENTS && err == seL4_NoError; ++f) {
                if (i != fb_service && i != fb_clients[f]) continue;
                uint32_t peer = i == fb_service ? fb_clients[f] : fb_service;
                seL4_Word slot = PD_CNODE_SLOT_FB_PEER_NOTIFY + (i == fb_service ? f : 0);
                err = seL4_CNode_Mint(pd_cnode, slot, pd->cnode_size_bits,
                    seL4_CapInitThreadCNode, g_pd_notifications[peer], 64u,
                    seL4_CapRights_new(0, 0, 0, 1), (seL4_Word)1u << f);
                if (err != seL4_NoError) break;
                seL4_Word copy = ut_alloc_slot();
                if (copy == seL4_CapNull) { err = seL4_NotEnoughMemory; break; }
                err = seL4_CNode_Copy(seL4_CapInitThreadCNode, copy, 64u,
                    seL4_CapInitThreadCNode, g_framebuffer_frames[f], 64u, seL4_AllRights);
                if (err == seL4_NoError)
                    err = pd_vspace_map_device_frame(vspace, copy,
                        AOS_FB_SHMEM_VA + f * AOS_FB_CLIENT_STRIDE);
            }
            if (i == fb_service) {
                for (uint32_t f = 0; f < AOS_FB_ARENA_FRAMES && err == seL4_NoError; ++f)
                    err = pd_vspace_map_device_frame(vspace, g_framebuffer_arena[f],
                        AOS_FB_ARENA_VA + f * AOS_FB_CLIENT_STRIDE);
            }
            if (err != seL4_NoError) {
                dbg_puts("[rt] framebuffer queue/capability grant failed; refusing PD start\n");
                continue;
            }
        }
#endif
        if (serial_virt_index != SYSTEM_MAX_PDS) {
            seL4_Error signal_err = seL4_NoError;
            if (pd_is_guest_vmm(pd) || pd->self_svc_id == SVC_ID_CC_PD ||
                pd->self_svc_id == SVC_ID_OPERATOR_SESSION) {
                seL4_Word badge = pd_is_guest_vmm(pd) ?
                    (1u << (pd_is_secondary_guest_vmm(pd) ? 1u : 0u)) :
                    pd->self_svc_id == SVC_ID_OPERATOR_SESSION ?
                    SERIAL_VIRT_OPERATOR_WAKE_BADGE : SERIAL_VIRT_FRONTEND_WAKE_BADGE;
                signal_err = seL4_CNode_Mint(pd_cnode,
                    PD_CNODE_SLOT_SERIAL_VIRT_NOTIFY, pd->cnode_size_bits,
                    seL4_CapInitThreadCNode, g_pd_notifications[serial_virt_index],
                    64u, seL4_CapRights_new(0, 0, 0, 1), badge);
            } else if (pd->self_svc_id == SVC_ID_SERIAL_VIRT) {
                for (uint32_t v = 0; v < sys->pd_count && signal_err == seL4_NoError; v++) {
                    if (!pd_is_guest_vmm(&sys->pds[v]) &&
                        sys->pds[v].self_svc_id != SVC_ID_OPERATOR_SESSION) continue;
                    seL4_Word slot = sys->pds[v].self_svc_id == SVC_ID_OPERATOR_SESSION ?
                        PD_CNODE_SLOT_SERIAL_OPERATOR_NOTIFY : pd_is_secondary_guest_vmm(&sys->pds[v]) ?
                        PD_CNODE_SLOT_SERIAL_SECONDARY_NOTIFY :
                        PD_CNODE_SLOT_SERIAL_PRIMARY_NOTIFY;
                    signal_err = seL4_CNode_Mint(pd_cnode, slot, pd->cnode_size_bits,
                        seL4_CapInitThreadCNode, g_pd_notifications[v], 64u,
                        seL4_CapRights_new(0, 0, 0, 1), SERIAL_VIRT_VMM_WAKE_BADGE);
                }
            }
            if (signal_err != seL4_NoError) {
                dbg_puts("[rt] serial signal grant failed; refusing PD start\n");
                continue;
            }
            if (pd->self_svc_id == SVC_ID_OPERATOR_SESSION &&
                seL4_CNode_Copy(pd_cnode, PD_CNODE_SLOT_OPERATOR_WAIT, pd->cnode_size_bits,
                    seL4_CapInitThreadCNode, g_pd_notifications[i], 64u,
                    seL4_CapRights_new(0, 0, 1, 0)) != seL4_NoError) {
                dbg_puts("[rt] operator receive grant failed; refusing PD start\n");
                continue;
            }
        }

        if (native_net_index != SYSTEM_MAX_PDS && net_virt_index != SYSTEM_MAX_PDS &&
            (i == native_net_index || i == net_virt_index)) {
            uint32_t peer = i == native_net_index ? net_virt_index : native_net_index;
            seL4_Word slot = i == native_net_index ? PD_CNODE_SLOT_NET_VIRT_NOTIFY :
                PD_CNODE_SLOT_NET_NATIVE_NOTIFY;
            if (seL4_CNode_Mint(pd_cnode, slot, pd->cnode_size_bits,
                    seL4_CapInitThreadCNode, g_pd_notifications[peer], 64u,
                    seL4_CapRights_new(0, 0, 0, 1), NET_VIRT_NATIVE_WAKE_BADGE) != seL4_NoError) {
                dbg_puts("[rt] native network wake grant failed; refusing PD start\n");
                continue;
            }
            if (i == native_net_index &&
                seL4_CNode_Copy(pd_cnode, PD_CNODE_SLOT_NATIVE_NET_WAIT, pd->cnode_size_bits,
                    seL4_CapInitThreadCNode, g_pd_notifications[i], 64u,
                    seL4_CapRights_new(0, 0, 1, 0)) != seL4_NoError) {
                dbg_puts("[rt] native network wait grant failed; refusing PD start\n");
                continue;
            }
        }

        if (blk_virt_index != SYSTEM_MAX_PDS) {
            seL4_Error signal_err = seL4_NoError;
            if (pd_is_guest_vmm(pd)) {
                signal_err = seL4_CNode_Mint(pd_cnode,
                    PD_CNODE_SLOT_BLK_VIRT_NOTIFY, pd->cnode_size_bits,
                    seL4_CapInitThreadCNode, g_pd_notifications[blk_virt_index],
                    64u, seL4_CapRights_new(0, 0, 0, 1),
                    1u << (pd_is_secondary_guest_vmm(pd) ? 1u : 0u));
            } else if (pd->self_svc_id == SVC_ID_BLK_VIRT) {
                for (uint32_t v = 0; v < sys->pd_count && signal_err == seL4_NoError; v++) {
                    if (!pd_is_guest_vmm(&sys->pds[v])) continue;
                    seL4_Word slot = pd_is_secondary_guest_vmm(&sys->pds[v]) ?
                        PD_CNODE_SLOT_BLK_SECONDARY_NOTIFY : PD_CNODE_SLOT_BLK_PRIMARY_NOTIFY;
                    signal_err = seL4_CNode_Mint(pd_cnode, slot, pd->cnode_size_bits,
                        seL4_CapInitThreadCNode, g_pd_notifications[v], 64u,
                        seL4_CapRights_new(0, 0, 0, 1), BLK_VIRT_VMM_WAKE_BADGE);
                }
            }
            if (signal_err != seL4_NoError) {
                dbg_puts("[rt] block signal grant failed; refusing PD start\n");
                continue;
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
            if (pd_is_guest_vmm(pd) &&
                (ep_spec->service_id == SVC_ID_NET_VIRT ||
                 ep_spec->service_id == SVC_ID_BLK_VIRT ||
                 ep_spec->service_id == SVC_ID_SERIAL_VIRT)) {
                badge = virt_client_badge(pd_is_secondary_guest_vmm(pd) ? 1u : 0u);
            } else if (pd->self_svc_id == SVC_ID_NATIVE_RUST_PROBE &&
                       ep_spec->service_id == SVC_ID_NET_VIRT) {
                badge = VIRT_NET_BADGE_NATIVE;
            } else if (pd->self_svc_id == SVC_ID_CC_PD &&
                       ep_spec->service_id == SVC_ID_SERIAL_VIRT) {
                badge = SERIAL_VIRT_FRONTEND_BADGE;
            } else if (pd->self_svc_id == SVC_ID_OPERATOR_SESSION &&
                       ep_spec->service_id == SVC_ID_SERIAL_VIRT) {
                badge = SERIAL_VIRT_OPERATOR_BADGE;
            }
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
            seL4_Error df_err;

            /*
             * The root task already retyped and mapped PL011 for bounded
             * bootstrap output.  Transfer that exact cap to serial_pd:
             * unmap it from root, map it in the driver VSpace, then move the
             * cap into the driver's CSpace.  Retyping a second frame would
             * leave an illicit root alias and fail once the 4K device untyped
             * is exhausted.
             */
            if (name_eq(pd->name, "serial_pd") &&
                df->paddr == AGENTOS_UART_PA &&
                g_uart_frame_cap != seL4_CapNull) {
                (void)seL4_ARCH_Page_Unmap(g_uart_frame_cap);
                g_uart_dr = (volatile uint32_t *)0;
                g_uart_fr = (volatile uint32_t *)0;

                df_err = pd_vspace_map_device_frame(vspace,
                                                     g_uart_frame_cap,
                                                     AGENTOS_UART_VA);
                seL4_Error move_err = seL4_CNode_Move(
                    pd_cnode,
                    (seL4_Word)df->cnode_slot,
                    (seL4_Word)pd->cnode_size_bits,
                    seL4_CapInitThreadCNode,
                    (seL4_Word)g_uart_frame_cap,
                    64u);
                if (move_err != seL4_NoError) {
                    (void)seL4_CNode_Delete(seL4_CapInitThreadCNode,
                                            (seL4_Word)g_uart_frame_cap,
                                            64u);
                    df_err = move_err;
                }
                g_uart_frame_cap = seL4_CapNull;
            } else {
                df_err = ut_alloc_device_frame(
                    (seL4_Word)df->paddr,
                    pd_cnode,
                    (seL4_Word)df->cnode_slot
                );
            }
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
         * maintain mappings for each profile-backed VMM guest RAM window. */
        for (uint8_t j = 0u; j < pd->mr_count; j++) {
            const memory_region_desc_t *mr = &pd->memory_regions[j];
            seL4_Error mr_err;
#if defined(__aarch64__)
            if (pd_is_guest_vmm(pd) &&
                name_eq(mr->name, "guest_ram")) {
                /*
                 * Guest RAM frames were reserved before PD ELF loading, while
                 * large aligned untypeds were still available. Fail fast if a
                 * reservation is missing; never enter the hour-scale 4 KiB
                 * fallback for GiB-sized guests.
                 * Emulated VirtIO translates every queue and payload GPA.
                 */
                const guest_ram_reservation_t *reservation =
                    guest_ram_reservation_for(i, j);
                if (reservation == NULL) {
                    mr_err = seL4_NotEnoughMemory;
                } else {
                    seL4_Word guest_gpa =
                        !pd_is_secondary_guest_vmm(pd)
                            ? AOS_PRIMARY_GUEST_GPA_BASE
                            : AOS_SECONDARY_GUEST_GPA_BASE;
                    mr_err = map_guest_ram_reservation(
                        reservation, vspace, guest_vspace,
                        (seL4_Word)mr->vaddr, guest_gpa,
                        (int)mr->writable);
                }
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
                dbg_puts(" err=");
                dbg_hex((seL4_Word)mr_err);
                dbg_puts("\n");
            }
        }

        /* Map the contract transfer page, never the UART frame, into serial
         * clients that emit boot diagnostics or console proof markers. */
        if (g_serial_shmem_frame_cap != seL4_CapNull &&
            (name_eq(pd->name, "serial_pd") ||
             name_eq(pd->name, "log_drain") ||
             pd_is_guest_vmm(pd) ||
             name_eq(pd->name, "cc_pd") ||
             name_eq(pd->name, "net_virt") ||
             name_eq(pd->name, "blk_virt") ||
             name_eq(pd->name, "serial_virt") ||
             name_eq(pd->name, "native_rust_client") ||
             name_eq(pd->name, "framebuffer_client0") ||
             name_eq(pd->name, "framebuffer_client1") ||
             name_eq(pd->name, "test_runner"))) {
            seL4_Word serial_copy = ut_alloc_slot();
            seL4_Error serial_err = seL4_NotEnoughMemory;
            if (serial_copy != seL4_CapNull) {
                serial_err = seL4_CNode_Copy(
                    seL4_CapInitThreadCNode, serial_copy, 64u,
                    seL4_CapInitThreadCNode, g_serial_shmem_frame_cap, 64u,
                    seL4_AllRights);
                if (serial_err == seL4_NoError) {
                    serial_err = pd_vspace_map_device_frame(
                        vspace, (seL4_CPtr)serial_copy, SERIAL_SHMEM_VA);
                }
            }
            dbg_puts("[rt] serial transfer page map err=");
            dbg_hex((seL4_Word)serial_err);
            dbg_puts("\n");
        }

        /* ── 4g.4.6b: Map GICv2 vCPU interface for VMM guests ───────────── */
#if defined(__aarch64__)
        /*
         * QEMU virt exposes a GICv2 CPU interface to the guest at 0x08010000.
         * On seL4 this is backed by the hardware virtual CPU interface frame
         * at 0x08040000. Without this pass-through mapping the guest faults as
         * soon as it writes GICC_PMR during IRQ setup.
         */
        if (g_gic_vcpu_frame_cap != seL4_CapNull &&
            pd_is_guest_vmm(pd)) {
            seL4_Word gic_copy = ut_alloc_slot();
            seL4_Error gic_err = seL4_NotEnoughMemory;
            if (gic_copy != seL4_CapNull) {
                gic_err = seL4_CNode_Copy(
                    seL4_CapInitThreadCNode, gic_copy,               64u,
                    seL4_CapInitThreadCNode, g_gic_vcpu_frame_cap,   64u,
                    seL4_AllRights);
            }
            dbg_puts("[rt] VMM GIC vCPU CNode_Copy err=");
            dbg_hex((seL4_Word)gic_err);
            dbg_puts("\n");
            if (gic_err == seL4_NoError) {
                gic_err = pd_vspace_map_device_frame(guest_vspace,
                                                      (seL4_CPtr)gic_copy,
                                                      GIC_VCPU_IF_VA);
            }
            dbg_puts("[rt] VMM GIC vCPU map err=");
            dbg_hex((seL4_Word)gic_err);
            dbg_puts("\n");
        }
#endif

        /* ── 4g.4.6c: Give virtio_blk sole access to host block hardware ─── */
#if defined(__aarch64__)
        if (name_eq(pd->name, "virtio_blk") &&
            g_host_blk_mmio_frame_cap != seL4_CapNull) {
            seL4_Word blk_mmio_copy = ut_alloc_slot();
            seL4_Error blk_err = seL4_NotEnoughMemory;
            if (blk_mmio_copy != seL4_CapNull) {
                blk_err = seL4_CNode_Copy(
                    seL4_CapInitThreadCNode, blk_mmio_copy, 64u,
                    seL4_CapInitThreadCNode, g_host_blk_mmio_frame_cap, 64u,
                    seL4_AllRights);
                if (blk_err == seL4_NoError) {
                    blk_err = pd_vspace_map_device_frame(
                        vspace, (seL4_CPtr)blk_mmio_copy,
                        AGENTOS_HOST_BLK_MMIO_VA);
                }
            }
            dbg_puts("[rt] virtio_blk host MMIO map err=");
            dbg_hex((seL4_Word)blk_err);
            dbg_puts("\n");
        }

        if (name_eq(pd->name, "virtio_blk") &&
            g_host_secondary_blk_mmio_frame_cap != seL4_CapNull) {
            seL4_Word blk_mmio_copy = ut_alloc_slot();
            seL4_Error blk_err = seL4_NotEnoughMemory;
            if (blk_mmio_copy != seL4_CapNull) {
                blk_err = seL4_CNode_Copy(
                    seL4_CapInitThreadCNode, blk_mmio_copy, 64u,
                    seL4_CapInitThreadCNode,
                    g_host_secondary_blk_mmio_frame_cap, 64u,
                    seL4_AllRights);
                if (blk_err == seL4_NoError) {
                    blk_err = pd_vspace_map_device_frame(
                        vspace, (seL4_CPtr)blk_mmio_copy,
                        AGENTOS_HOST_SECONDARY_BLK_PAGE_VA);
                }
            }
            dbg_puts("[rt] virtio_blk secondary host MMIO map err=");
            dbg_hex((seL4_Word)blk_err);
            dbg_puts("\n");
        }

        /* The driver DMA window is shared by virtio_blk and blk_virt only.
         * No VMM maps it: guest block data reaches the driver through the
         * blk_virt queues (docs/TCB.md invariant 2). */
        if (g_blk_shared_frame_cap != seL4_CapNull &&
            (name_eq(pd->name, "virtio_blk") ||
             name_eq(pd->name, "blk_virt"))) {
            seL4_Word blk_shared_copy = ut_alloc_slot();
            seL4_Error blk_err = seL4_NotEnoughMemory;
            if (blk_shared_copy != seL4_CapNull) {
                blk_err = seL4_CNode_Copy(
                    seL4_CapInitThreadCNode, blk_shared_copy, 64u,
                    seL4_CapInitThreadCNode, g_blk_shared_frame_cap, 64u,
                    seL4_AllRights);
                if (blk_err == seL4_NoError) {
                    blk_err = pd_vspace_map_device_frame(
                        vspace, (seL4_CPtr)blk_shared_copy,
                        AGENTOS_BLK_SHARED_VA);
                }
            }
            dbg_puts("[rt] ");
            dbg_puts(pd->name);
            dbg_puts(" blk shared map err=");
            dbg_hex((seL4_Word)blk_err);
            dbg_puts("\n");
        }

        /* blk_virt maps the region; each VMM maps only its client page.
         * Keep the private RAM disk and other clients out of its VSpace.
         * Each frame cap is copied because a frame cap maps exactly once. */
        if (g_blk_virt_frame_caps[0] != seL4_CapNull &&
            (name_eq(pd->name, "blk_virt") || pd_is_guest_vmm(pd))) {
            seL4_Error blk_err = seL4_NoError;
            for (uint32_t f = 0u; f < AOS_BLK_SHMEM_FRAMES &&
                                  blk_err == seL4_NoError; f++) {
                if (pd_is_guest_vmm(pd) &&
                    f != AOS_BLK_CLIENT_BASE / AOS_BLK_SHMEM_FRAME_SIZE +
                             (pd_is_secondary_guest_vmm(pd) ? 1u : 0u)) {
                    continue;
                }
                seL4_Word frame_copy = ut_alloc_slot();
                blk_err = seL4_NotEnoughMemory;
                if (frame_copy != seL4_CapNull) {
                    blk_err = seL4_CNode_Copy(
                        seL4_CapInitThreadCNode, frame_copy, 64u,
                        seL4_CapInitThreadCNode, g_blk_virt_frame_caps[f],
                        64u, seL4_AllRights);
                    if (blk_err == seL4_NoError) {
                        blk_err = pd_vspace_map_device_frame(
                            vspace, (seL4_CPtr)frame_copy,
                            AOS_BLK_SHMEM_VA +
                                (seL4_Word)f * AOS_BLK_SHMEM_FRAME_SIZE);
                    }
                }
            }
            dbg_puts("[rt] ");
            dbg_puts(pd->name);
            dbg_puts(" blk_virt shared region map err=");
            dbg_hex((seL4_Word)blk_err);
            dbg_puts("\n");
        }

        /* Only serial_virt sees both guest pages, operator and frontend. */
        if (serial_virt_index != SYSTEM_MAX_PDS &&
            (pd_is_guest_vmm(pd) || pd->self_svc_id == SVC_ID_CC_PD ||
             pd->self_svc_id == SVC_ID_OPERATOR_SESSION ||
             pd->self_svc_id == SVC_ID_SERIAL_VIRT)) {
            seL4_Error serial_err = seL4_NoError;
            for (uint32_t f = 0; f < AOS_SERIAL_FRAMES && serial_err == seL4_NoError; f++) {
                if (pd_is_guest_vmm(pd) &&
                    f != (pd_is_secondary_guest_vmm(pd) ? 1u : 0u)) continue;
                if (pd->self_svc_id == SVC_ID_CC_PD && f != AOS_SERIAL_FRONTEND_FRAME) continue;
                if (pd->self_svc_id == SVC_ID_OPERATOR_SESSION && f != SERIAL_VIRT_OPERATOR_CLIENT) continue;
                seL4_Word copy = ut_alloc_slot();
                serial_err = seL4_NotEnoughMemory;
                if (copy != seL4_CapNull) {
                    serial_err = seL4_CNode_Copy(seL4_CapInitThreadCNode, copy, 64u,
                        seL4_CapInitThreadCNode, g_serial_virt_frames[f], 64u, seL4_AllRights);
                    if (serial_err == seL4_NoError)
                        serial_err = pd_vspace_map_device_frame(vspace, copy,
                            AOS_SERIAL_SHMEM_VA + f * AOS_SERIAL_FRAME_SIZE);
                }
            }
            if (serial_err != seL4_NoError) {
                dbg_puts("[rt] serial page mapping failed; refusing PD start\n");
                continue;
            }
        }

        /* VMMs map their own queue page, the NIC driver maps its transfer
         * page, and only net_virt maps both tiers. */
        if (g_net_shared_frame_caps[0] != seL4_CapNull &&
            (name_eq(pd->name, "net_pd") ||
             name_eq(pd->name, "net_virt") ||
             pd->self_svc_id == SVC_ID_NATIVE_RUST_PROBE ||
             pd_is_guest_vmm(pd))) {
            seL4_Error net_err = seL4_NoError;
            for (uint32_t f = 0u; f < AOS_NET_SHMEM_FRAMES && net_err == seL4_NoError; ++f) {
                if (pd_is_guest_vmm(pd) &&
                    f != (pd_is_secondary_guest_vmm(pd) ? 1u : 0u)) continue;
                if (name_eq(pd->name, "net_pd") && f != AOS_NET_DRIVER_FRAME) continue;
                if (pd->self_svc_id == SVC_ID_NATIVE_RUST_PROBE && f != AOS_NET_NATIVE_CLIENT) continue;
                seL4_Word net_shared_copy = ut_alloc_slot();
                net_err = seL4_NotEnoughMemory;
                if (net_shared_copy != seL4_CapNull) {
                    net_err = seL4_CNode_Copy(
                        seL4_CapInitThreadCNode, net_shared_copy, 64u,
                        seL4_CapInitThreadCNode, g_net_shared_frame_caps[f], 64u,
                        seL4_AllRights);
                    if (net_err == seL4_NoError) {
                        net_err = pd_vspace_map_device_frame(
                            vspace, (seL4_CPtr)net_shared_copy,
                            AGENTOS_NET_SHARED_VA + (seL4_Word)f * AOS_NET_SHMEM_FRAME_SIZE);
                    }
                }
            }
            dbg_puts("[rt] ");
            dbg_puts(pd->name);
            dbg_puts(" agentOS net shared map err=");
            dbg_hex((seL4_Word)net_err);
            dbg_puts("\n");
            if (net_err != seL4_NoError) continue;
        }

        if (name_eq(pd->name, "net_pd")) {
            seL4_Error net_err = seL4_NotEnoughMemory;
            if (g_host_net_mmio_frame_cap != seL4_CapNull) {
                seL4_Word net_mmio_copy = ut_alloc_slot();
                if (net_mmio_copy != seL4_CapNull) {
                    net_err = seL4_CNode_Copy(
                        seL4_CapInitThreadCNode, net_mmio_copy, 64u,
                        seL4_CapInitThreadCNode,
                        g_host_net_mmio_frame_cap, 64u,
                        seL4_AllRights);
                    if (net_err == seL4_NoError) {
                        net_err = pd_vspace_map_device_frame(
                            vspace, (seL4_CPtr)net_mmio_copy,
                            AGENTOS_HOST_NET_MMIO_VA);
                    }
                }
            }
            dbg_puts("[rt] net_pd host MMIO map err=");
            dbg_hex((seL4_Word)net_err);
            dbg_puts("\n");

            net_err = seL4_NotEnoughMemory;
            if (g_net_dma_frame_cap != seL4_CapNull) {
                seL4_Word net_dma_copy = ut_alloc_slot();
                if (net_dma_copy != seL4_CapNull) {
                    net_err = seL4_CNode_Copy(
                        seL4_CapInitThreadCNode, net_dma_copy, 64u,
                        seL4_CapInitThreadCNode, g_net_dma_frame_cap, 64u,
                        seL4_AllRights);
                    if (net_err == seL4_NoError) {
                        net_err = pd_vspace_map_device_frame(
                            vspace, (seL4_CPtr)net_dma_copy,
                            AGENTOS_NET_HOST_DMA_VA);
                    }
                }
            }
            dbg_puts("[rt] net_pd private DMA map err=");
            dbg_hex((seL4_Word)net_err);
            dbg_puts("\n");
        }
#endif

        /* ── 4g.4.7: Set up VirtIO serial transport for cc_pd ───────────────── */
        /*
         * cc_pd uses VirtIO serial (bus.2 = PA 0x0A000400) as its host socket
         * bridge.  QEMU bridges it to build/cc_pd.sock via virtconsole.
         *
         * We map three resources into cc_pd's VSpace:
         *   1. Device page at PA 0x0A000000 (covers virtio-mmio slots 0-7) at
         *      CC_VIRTIO_MMIO_VA.  cc_pd probes slot 2 (offset +0x400).
         *   2. Three normal frames for VirtIO queue memory (desc/avail/used
         *      rings + TX/RX data buffers), mapped at fixed cc_pd CPU VAs.
         *   3. A versioned startup record at CC_VIRTIO_STARTUP_VA carrying
         *      only the three device-visible physical addresses.
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
                    ve = pd_vspace_map_device_frame(vspace, cc_virtio_cap,
                                                    CC_VIRTIO_MMIO_VA);
                }
                dbg_puts("[rt] cc_pd VirtIO page err=");
                dbg_hex((seL4_Word)ve);
                dbg_puts("\n");
            }

            /* 2. Allocate queue + buffer frames and map fixed CPU virtual
             * addresses independently from their device-visible PAs. */
            seL4_Word vq_pas[3] = {0u, 0u, 0u};
            static const seL4_Word vq_vas[3] = {
                CC_VIRTIO_QUEUE_VA,
                CC_VIRTIO_TX_BUFFER_VA,
                CC_VIRTIO_RX_BUFFER_VA,
            };
            for (uint32_t vqp = 0u; vqp < 3u; vqp++) {
                seL4_CPtr vq_cap = seL4_CapNull;
                seL4_Error ve = ut_alloc_cap(seL4_ARM_SmallPageObject, 0u, &vq_cap);
                if (ve == seL4_NoError) {
                    seL4_ARCH_Page_GetAddress_t r = seL4_ARCH_Page_GetAddress(vq_cap);
                    vq_pas[vqp] = r.paddr;
                    ve = pd_vspace_map_device_frame(vspace, vq_cap,
                                                    vq_vas[vqp]);
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
                        volatile cc_virtio_startup_t *sp =
                            (volatile cc_virtio_startup_t *)RT_VQ_SCRATCH_VA;
                        sp->magic = CC_VIRTIO_STARTUP_MAGIC;
                        sp->version = CC_VIRTIO_STARTUP_VERSION;
                        sp->queue_pa = (uint64_t)vq_pas[0];
                        sp->tx_buffer_pa = (uint64_t)vq_pas[1];
                        sp->rx_buffer_pa = (uint64_t)vq_pas[2];
                        AGENTOS_MEMORY_FENCE();
                        seL4_ARCH_Page_Unmap(cc_start_cap);
                        seL4_Error ve2 = pd_vspace_map_device_frame(
                            vspace, cc_start_cap, CC_VIRTIO_STARTUP_VA);
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

        }

        /* ── 4g.4.9: EventBus ring RAM region (agentos-gom) ─────────────────
         * Map a private 2 MB RAM region at EVENTBUS_RING_VA into the event_bus
         * PD's vspace so its ring is backed on target.  Without this,
         * eventbus_ring_vaddr stays 0 (only the host unit test set it) and
         * STATUS/INIT/PUBLISH return BAD_ARG.  VA must match EVENTBUS_RING_VA in
         * services/event-bus/event_bus.c. */
        if (name_eq(pd->name, "event_bus")) {
            seL4_Error re = pd_vspace_map_region(vspace,
                                                 0x30000000UL /* EVENTBUS_RING_VA */,
                                                 0x200000u    /* 2 MB (ring uses first 256 KB) */,
                                                 1            /* writable */);
            dbg_puts("[rt] event_bus ring map err=");
            dbg_hex((seL4_Word)re);
            dbg_puts("\n");
        }

#ifdef AGENTOS_LOG_RINGS
        if (provision_log_config(pd, i, vspace, pd_cnode, log_drain_index) != seL4_NoError) {
            dbg_puts("[rt] log provisioning failed; refusing PD start\n");
            return;
        }
#endif
        /* ── 4g.4.8: Startup record for parameterized PDs (agentos-3ev) ────── */
        /*
         * swap_slot, app_slot, wg_net, and (standalone) vibe_swap each read a
         * per-instance startup record at PD_STARTUP_RECORD_VA carrying their
         * slot id and peer/controller endpoint cap slots.  Provisioning it here
         * removes the TEMPORARY hard-coded slot 0 / NULL controller cap from
         * those wrappers.
         */
        if (pd_is_parameterized(pd->name)) {
            seL4_Error sr = provision_pd_startup_record(pd, vspace);
            dbg_puts("[rt] startup record for ");
            dbg_puts(pd->name);
            dbg_puts(" err=");
            dbg_hex((seL4_Word)sr);
            dbg_puts("\n");
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
#if defined(__aarch64__)
        if (guest_vspace != seL4_CapNull) {
            cap_acct_record(seL4_CapNull, guest_vspace,
                            seL4_ARM_VSpaceObject, i, pd->name);
        }
#endif
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
        if (pd_is_guest_vmm(pd)) {
            seL4_Error vm_err = setup_vmm_guest_vcpu(pd,
                                                      i,
                                                      pd_cnode,
                                                      guest_vspace,
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
#if defined(__x86_64__) && defined(AGENTOS_X86_VTX)
        if (pd_is_guest_vmm(pd)) {
            seL4_Error vm_err = setup_x86_vtx_proof(pd, i, pd_cnode,
                                                     tr.tcb_cap, vspace);
            if (vm_err != seL4_NoError) {
                dbg_puts("[rt] x86 VMX EPT proof provisioning FAILED err=");
                dbg_hex((seL4_Word)vm_err);
                dbg_puts("\n");
                return;
            }
            g_x86_vtx_proof_endpoint = self_ep;
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
                if (reg_err == seL4_NoError && inspect_view.thread_count < AOS_INSPECT_MAX_THREADS) {
                    aos_inspect_thread_t *t = &inspect_view.threads[inspect_view.thread_count++];
                    t->pd_index = i;
                    t->prio = pd->priority;
                    t->state = AOS_INSPECT_THR_UNKNOWN;
                    for (uint32_t n = 0; n + 1 < AOS_INSPECT_NAME_LEN && pd->name[n]; n++)
                        t->name[n] = (uint8_t)pd->name[n];
                    if (pd->self_svc_id == SVC_ID_CC_PD) inspect_cc_vspace = vspace;
                    if (pd->self_svc_id == SVC_ID_OPERATOR_SESSION) inspect_operator_vspace = vspace;
                }
            }
        }
    }

    /* Publish once, before yielding to PDs. CC receives only a read mapping;
     * the frame capability stays in root, and no inspection server is added. */
    if (inspect_cc_vspace != seL4_CapNull) {
        seL4_CPtr frame = seL4_CapNull;
        if (ut_alloc_cap(seL4_ARM_SmallPageObject, 0u, &frame) != seL4_NoError ||
            pd_vspace_map_device_frame(seL4_CapInitThreadVSpace, frame,
                                      RT_VQ_SCRATCH_VA) != seL4_NoError) {
            dbg_puts("[rt] inspect mapping failed; refusing partial boot\n");
            return;
        }
        seL4_CPtr readers[] = {inspect_cc_vspace, inspect_operator_vspace};
        for (unsigned i = 0; i < 2; i++) {
            if (!readers[i]) continue;
            seL4_CPtr reader = ut_alloc_slot();
            if (!reader || seL4_CNode_Copy(seL4_CapInitThreadCNode, reader, 64u,
                    seL4_CapInitThreadCNode, frame, 64u,
                    seL4_CapRights_new(0, 0, 1, 0)) != seL4_NoError ||
                pd_vspace_map_device_frame(readers[i], reader, AOS_INSPECT_BOOT_VA) != seL4_NoError) {
                dbg_puts("[rt] inspect reader mapping failed; refusing partial boot\n");
                return;
            }
        }
        ut_alloc_observe(&inspect_view.ut_total_bytes, &inspect_view.ut_used_bytes);
#if defined(__aarch64__)
        inspect_view.arch = AOS_INSPECT_ARCH_AARCH64;
        inspect_view.uart_pa = AGENTOS_UART_PA;
        inspect_view.virtio_net_ipa = AOS_VIRTIO_NET_GUEST_IPA;
        inspect_view.virtio_net_virq = AOS_VIRTIO_NET_VIRQ;
        inspect_view.gic_dist_pa = 0x08000000u;
        for (uint32_t i = 0; i < g_guest_ram_reservation_count; i++)
            inspect_view.guest_ram_bytes +=
                (uint64_t)g_guest_ram_reservations[i].frame_count << seL4_ARCH_LargePageBits;
#elif defined(__x86_64__)
        inspect_view.arch = AOS_INSPECT_ARCH_X86_64;
#elif defined(__riscv)
        inspect_view.arch = AOS_INSPECT_ARCH_RISCV64;
#endif
        _Static_assert(sizeof(aos_inspect_snapshot_t) <= 4096, "inspect fits one page");
        if (aos_inspect_fill((aos_inspect_snapshot_t *)RT_VQ_SCRATCH_VA,
                             &inspect_view) != AOS_INSPECT_OK) {
            dbg_puts("[rt] inspect observation invalid; refusing partial boot\n");
            return;
        }
        if (seL4_ARCH_Page_Unmap(frame) != seL4_NoError) return;
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

#if defined(AGENTOS_SEL4_TEST_IMAGE) && defined(__aarch64__)
    /* agentos-8f5: on aarch64 the contract-runner PD (test_runner.elf) emits the
     * authoritative TAP stream + TAP_DONE sentinel once the service PDs are in
     * their server loops.  The root task must NOT emit TAP_DONE here — doing so
     * makes run-tests tear down QEMU before any PD (incl. the runner) executes. */
    dbg_puts("[rt] test image: contract-runner PD will emit TAP after PD bringup\n");
#elif defined(AGENTOS_SEL4_TEST_IMAGE)
    /* Other arches (e.g. x86_64 reduced smoke) have no contract-runner PD wired
     * yet, so the root task emits the boot-proof TAP itself. */
    dbg_puts("TAP version 14\n");
    dbg_puts("ok 1 - root task booted current board topology\n");
    dbg_puts("1..1\n");
    dbg_puts("TAP_DONE:0\n");
#endif

    dbg_puts("[rt] boot complete — yielding to PDs\n");

#if defined(__x86_64__) && defined(AGENTOS_X86_VTX)
    /*
     * The VMM uses its self endpoint to report the exact exit observed after
     * VM entry.  Validate the complete small protocol before emitting the
     * qualification marker, then resume normal root fault handling.
     */
    if (g_x86_vtx_proof_endpoint == seL4_CapNull) {
        dbg_puts("[rt] x86 VMX EPT proof FAILED: endpoint unavailable\n");
        return;
    }
    {
        seL4_Word badge = 0u;
        seL4_MessageInfo_t tag =
            seL4_Wait(g_x86_vtx_proof_endpoint, &badge);
        seL4_Word status = seL4_GetMR(0);
        seL4_Word reason = seL4_GetMR(1);
        seL4_Word rip = seL4_GetMR(2);
        seL4_Word instruction_len = seL4_GetMR(3);
#ifdef AGENTOS_X86_FIRMWARE_RESET
        if (seL4_MessageInfo_get_label(tag) == AOS_X86_VTX_PROOF_LABEL &&
            seL4_MessageInfo_get_length(tag) == AOS_X86_FIRMWARE_REPORT_WORDS) {
            seL4_Word counters[6];
            for (unsigned i=0; i<6; i++) counters[i]=seL4_GetMR(4+i);
            seL4_Word snapshot[AOS_X86_FIRMWARE_SNAPSHOT_WORDS];
            for (unsigned i=0; i<AOS_X86_FIRMWARE_SNAPSHOT_WORDS; i++)
                snapshot[i]=seL4_GetMR(10+i);
            seL4_Word chain[AOS_X86_FIRMWARE_CHAIN_WORDS];
            for (unsigned i=0; i<AOS_X86_FIRMWARE_CHAIN_WORDS; i++)
                chain[i]=seL4_GetMR(10+AOS_X86_FIRMWARE_SNAPSHOT_WORDS+i);
            seL4_Word boot[4];
            for (unsigned i=0; i<4; i++) boot[i]=seL4_GetMR(116+i);
            dbg_puts("[rt] firmware timer exits="); dbg_hex(counters[0]);
            dbg_puts(" injections="); dbg_hex(counters[1]);
            dbg_puts(" eois="); dbg_hex(counters[2]);
            dbg_puts(" rate_shift="); dbg_hex(counters[3]);
            dbg_puts(" tsc_hz="); dbg_hex(counters[4]);
            dbg_puts(" halt_exits="); dbg_hex(counters[5]); dbg_puts("\n");
            dbg_puts("[rt] firmware boot bytes kernel="); dbg_hex(boot[0]);
            dbg_puts(" initrd="); dbg_hex(boot[1]);
            dbg_puts(" cmdline="); dbg_hex(boot[2]);
            dbg_puts(" last_qualification="); dbg_hex(boot[3]); dbg_puts("\n");
            if (status == AOS_X86_VTX_PROOF_FAIL && reason == 0x425544u) {
              for (unsigned set=0; set<2; set++) {
                const seL4_Word *view=snapshot+set*AOS_X86_FIRMWARE_SNAPSHOT_SET_WORDS;
                dbg_puts(set ? "[rt] firmware observed wait exit\n" :
                               "[rt] firmware budget exit\n");
                for (unsigned region=0; region<2; region++) {
                    unsigned count=region ? AOS_X86_FIRMWARE_STACK_WORDS :
                                            AOS_X86_FIRMWARE_CODE_WORDS;
                    unsigned start=4+(region ? AOS_X86_FIRMWARE_CODE_WORDS : 0);
                    dbg_puts(region ? "[rt] firmware stack snapshot\n" :
                                      "[rt] firmware code snapshot\n");
                    for (unsigned i=0; i<count; i++) {
                        if (!(view[2+region] & (UINT64_C(1) << i))) break;
                        dbg_puts("[rt] snapshot "); dbg_hex(view[region]+8u*i);
                        dbg_puts(" = "); dbg_hex(view[start+i]); dbg_puts("\n");
                    }
                }
              }
              seL4_Word frame=chain[0];
              for (unsigned i=0; i<AOS_X86_FIRMWARE_CHAIN_FRAMES && i<chain[1]; i++) {
                  dbg_puts("[rt] firmware observed frame "); dbg_hex(frame);
                  dbg_puts(" return "); dbg_hex(chain[3+2*i]);
                  dbg_puts(" next "); dbg_hex(chain[2+2*i]); dbg_puts("\n");
                  frame=chain[2+2*i];
              }
            }
        }
#endif
        if (seL4_MessageInfo_get_label(tag) == AOS_X86_VTX_PROOF_LABEL &&
#ifdef AGENTOS_X86_FIRMWARE_RESET
            seL4_MessageInfo_get_length(tag) == AOS_X86_FIRMWARE_REPORT_WORDS &&
#else
            seL4_MessageInfo_get_length(tag) == 4u &&
#endif
#ifdef AGENTOS_X86_FIRMWARE_RESET
            status == AOS_X86_VTX_FIRMWARE_CONFIG &&
            reason == 30u && rip <= 0xffffffffu &&
            (instruction_len >> 16) == 0x511u && (instruction_len & (1u << 4))) {
            dbg_puts("[rt] x86 OVMF PCI configuration and fw_cfg string exit verified\n");
            dbg_puts("[rt] firmware exit reason="); dbg_hex(reason);
            dbg_puts(" linear RIP="); dbg_hex(rip);
            dbg_puts(" qualification="); dbg_hex(instruction_len); dbg_puts("\n");
#else
#ifdef AGENTOS_X86_FIRMWARE_MODES
            status == AOS_X86_VTX_MODES_PASS &&
#else
            status == AOS_X86_VTX_PROOF_PASS &&
#endif
            (reason & 0xffffu) == AOS_X86_VTX_HLT_EXIT_REASON &&
            rip == AOS_X86_VTX_GUEST_RIP &&
            instruction_len == AOS_X86_VTX_HLT_INSTRUCTION_LEN) {
#ifdef AGENTOS_X86_FIRMWARE_MODES
            dbg_puts("[rt] x86 VMX real protected long entry modes verified\n");
#else
            dbg_puts("[rt] x86 VMX EPT HLT exit verified\n");
#endif
#endif
        } else {
            dbg_puts("[rt] x86 VMX EPT proof FAILED status=");
            dbg_hex(status);
            dbg_puts(" reason=");
            dbg_hex(reason);
            dbg_puts(" rip=");
            dbg_hex(rip);
            dbg_puts(" len=");
            dbg_hex(instruction_len);
            dbg_puts("\n");
            return;
        }
    }
#endif

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
#ifdef ROOT_FAULT_PROBE
        serial_log_t probe_log = {0};
        seL4_CPtr probe_serial_frame = ut_alloc_slot();
        if (probe_serial_frame != seL4_CapNull &&
            seL4_CNode_Copy(seL4_CapInitThreadCNode, probe_serial_frame, 64u,
                           seL4_CapInitThreadCNode, g_serial_shmem_frame_cap,
                           64u, seL4_AllRights) == seL4_NoError &&
            pd_vspace_map_device_frame(seL4_CapInitThreadVSpace,
                                      probe_serial_frame, AGENTOS_SERIAL_SHMEM_VA) == seL4_NoError) {
            probe_log.ep = ep_alloc_for_service(SVC_ID_SERIAL);
        }
#endif
        dbg_puts("[rt] parking on fault_ep\n");
        for (;;) {
            seL4_Word badge = 0u;
            seL4_MessageInfo_t tag = seL4_Wait(g_fault_ep, &badge);
            seL4_Word label = seL4_MessageInfo_get_label(tag);
#ifdef ROOT_FAULT_PROBE
            if (badge == ROOT_PROBE_BADGE && label == seL4_Fault_VMFault &&
                seL4_MessageInfo_get_length(tag) >= seL4_VMFault_Length &&
                seL4_GetMR(seL4_VMFault_Addr) == ROOT_PROBE_ADDRESS &&
                seL4_GetMR(seL4_VMFault_PrefetchFault) == 0u &&
                ((seL4_GetMR(seL4_VMFault_FSR) >> 6u) & 1u) == ROOT_PROBE_WRITE) {
                serial_log_puts(&probe_log, ROOT_PROBE_MESSAGE);
            }
#endif
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
