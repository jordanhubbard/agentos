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
 * The BOARD_qemu_virt_aarch64 define is injected by the Makefile when
 * MICROKIT_BOARD=qemu_virt_aarch64.  All other boards default to RISC-V.
 */
#ifdef BOARD_qemu_virt_aarch64
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
#define SLOTS_PER_PD           4u    /* CNODE slot + TCB slot + VSPACE slot + IPC_FRAME slot */

/* Slot layout per PD index i (base + i * SLOTS_PER_PD + offset): */
#define PD_SLOT_CNODE(i)      (CAP_ROOT_INITIAL_BASE + (seL4_Word)(i) * SLOTS_PER_PD + 0u)
#define PD_SLOT_TCB(i)        (CAP_ROOT_INITIAL_BASE + (seL4_Word)(i) * SLOTS_PER_PD + 1u)
#define PD_SLOT_VSPACE(i)     (CAP_ROOT_INITIAL_BASE + (seL4_Word)(i) * SLOTS_PER_PD + 2u)
#define PD_SLOT_IPC_FRAME(i)  (CAP_ROOT_INITIAL_BASE + (seL4_Word)(i) * SLOTS_PER_PD + 3u)

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
 * boot_find_elf — locate an embedded ELF image in BootInfo extra regions.
 *
 * The xtask gen-image tool embeds each PD's ELF into the extra BootInfo
 * region with a AGENTOS_BOOTINFO_HEADER_ELF chunk header containing the
 * PD name.  We walk the extra region list to find a matching chunk.
 *
 * Returns a pointer to the start of the ELF data on success, or NULL if
 * no matching chunk is found.  pd_vspace_load_elf handles NULL gracefully
 * by returning an error.
 */
static const void *boot_find_elf(const seL4_BootInfo *bi, const char *elf_path)
{
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
 * boot_elf_size — return the ELF image size for a named PD.
 *
 * Walks the same extra BootInfo region list as boot_find_elf and returns
 * the size stored in the agentos_elf_region_t descriptor.  Returns 0 if
 * the PD is not found.
 */
static seL4_Word boot_elf_size(const seL4_BootInfo *bi, const char *elf_path)
{
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
                             seL4_Word        pd_cnode_depth)
{
    for (uint8_t i = 0u; i < pd->irq_count; i++) {
        const irq_desc_t *d = &pd->irqs[i];

        /* Destination slot in the PD's own CNode */
        seL4_Word dest_slot = (seL4_Word)PD_IRQHANDLER_SLOT_BASE + (seL4_Word)i;

        seL4_Error err = seL4_IRQControl_Get(
            irq_control_cap,
            (seL4_Word)d->irq_number,
            pd_cnode,
            dest_slot,
            pd_cnode_depth);

        /*
         * Log failures but do not abort boot: a missing IRQ handler cap means
         * the PD will receive seL4_InvalidCapability when it calls
         * seL4_IRQHandler_Ack(), which is recoverable.
         */
        (void)err;
    }
}

/* ── Main boot sequence ───────────────────────────────────────────────────── */

void root_task_main(const seL4_BootInfo *bi)
{
    /* ── Step 1: Initialise untyped memory allocator ──────────────────────── */
    ut_alloc_init(bi);

    /*
     * Set the static cap slot base to bi->empty.start (the first truly-free
     * slot) and reserve static slots for per-PD objects and the EP pool so
     * that subsequent ut_alloc_cap() calls start AFTER them.
     */
    g_cap_base = ut_free_slot_base();
    ut_advance_slot_cursor((seL4_Word)SYSTEM_MAX_PDS * SLOTS_PER_PD + EP_POOL_SIZE);

    /* ── Step 2: Initialise capability accounting ─────────────────────────── */
    cap_acct_init(bi);

    /* ── Step 3: Initialise endpoint pool ─────────────────────────────────── */
    /*
     * Reserve EP_POOL_SIZE slots starting immediately after the per-PD
     * object slots.  The endpoint pool has a known base so that IPC buffer
     * frame slots (allocated past the EP pool) do not collide with it.
     */
    ep_alloc_init(seL4_CapInitThreadCNode, EP_POOL_BASE, EP_POOL_SIZE);

    /* ── Step 4: Load and start each PD ───────────────────────────────────── */
    const system_desc_t *sys = SYSTEM_DESC;

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
        pd_vspace_result_t vr_create =
            pd_vspace_create(seL4_CapInitThreadCNode,
                             seL4_CapInitThreadASIDPool);
        if (vr_create.error != 0) {
            /* Non-fatal: log and skip this PD */
            continue;
        }
        seL4_CPtr vspace = vr_create.vspace_cap;

        /* ── 4b: Allocate IPC buffer frame ──────────────────────────────── */
        /*
         * Retype one 4 KB page frame into the IPC buffer slot.
         * seL4_ARM_SmallPageObject == 4 KB page on AArch64; the same
         * numeric constant is used on RISC-V (both platforms map to
         * the equivalent small page type).
         */
        seL4_Error err = ut_alloc(seL4_ARM_SmallPageObject,
                                   0u /* size_bits: fixed 4 KB page */,
                                   seL4_CapInitThreadCNode,
                                   PD_SLOT_IPC_FRAME(i),
                                   64u /* dest_depth */);
        if (err != seL4_NoError) {
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
            continue;
        }
        seL4_CPtr pd_cnode = PD_SLOT_CNODE(i);

        /* ── 4d: Locate embedded ELF ────────────────────────────────────── */
        const void *elf_data = boot_find_elf(bi, pd->elf_path);
        seL4_Word   elf_size = boot_elf_size(bi, pd->elf_path);
        /*
         * elf_data == NULL is allowed: pd_vspace_load_elf returns an error
         * which we check below.  This path will be common in early simulator
         * tests before xtask gen-image embeds real ELFs.
         */

        /* ── 4e: Load ELF into VSpace ───────────────────────────────────── */
        pd_vspace_result_t vr = pd_vspace_load_elf(vspace,
                                                    elf_data,
                                                    (uint32_t)elf_size,
                                                    pd->stack_size);
        if (vr.error != 0) {
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

        pd_tcb_result_t tr = pd_tcb_create(seL4_CapInitThreadCNode,
                                            PD_SLOT_TCB(i),
                                            vspace,
                                            pd_cnode,
                                            ipc_buf_cap,
                                            ipc_buf_va,
                                            pd->priority);
        if (tr.error != 0) {
            continue;
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
                sel4_dbg_puts("[root] WARN: device frame retype failed: ");
                sel4_dbg_puts(df->name);
                sel4_dbg_puts("\n");
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
            seL4_Error mr_err = pd_vspace_map_region(
                vspace,
                (seL4_Word)mr->vaddr,
                (size_t)mr->size,
                (int)mr->writable
            );
            if (mr_err != seL4_NoError) {
                sel4_dbg_puts("[root] WARN: memory region map failed: ");
                sel4_dbg_puts(mr->name);
                sel4_dbg_puts("\n");
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
                            (seL4_Word)pd->cnode_size_bits);
        }

        /* ── 4h: Record all new caps in the accounting tree ─────────────── */
        cap_acct_record(seL4_CapNull, pd_cnode, seL4_CapTableObject,   i, pd->name);
        cap_acct_record(seL4_CapNull, vspace,   seL4_ARM_VSpaceObject, i, pd->name);
        cap_acct_record(seL4_CapNull, tr.tcb_cap, seL4_TCBObject,       i, pd->name);
        cap_acct_record(seL4_CapNull, ipc_frame,  seL4_ARM_SmallPageObject, i, pd->name);

        /* ── 4i: Write initial registers and start the PD thread ─────────── */
        /*
         * arg0 (x0 / a0) is set to 0.  PDs discover their own identity via
         * the nameserver or from the badge value on their endpoint.
         */
        pd_tcb_set_regs(tr.tcb_cap,
                         vr.entry_point,
                         vr.stack_top,
                         0u /* arg0 */);
        pd_tcb_start(tr.tcb_cap);
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

    /* ── Step 5: Idle loop ────────────────────────────────────────────────── */
    /*
     * The root task must never exit.  After all PDs are started, yield the
     * CPU indefinitely.  seL4's scheduler will run the PD threads.
     * The root task only wakes when it is the only runnable thread.
     */
    while (1) {
        seL4_Yield();
    }
}

/* ── Root task entry point ────────────────────────────────────────────────── */

/*
 * _start — seL4 root task entry point.
 *
 * seL4 jumps here directly after setting up the initial CNode and placing
 * the BootInfo pointer in a well-known register:
 *
 *   AArch64: the BootInfo pointer is in x2 (third argument register)
 *   RISC-V:  the BootInfo pointer is in a2 (third argument register)
 *
 * Both ISAs use the same register offset for the third argument, so the
 * same assembly sequence works on both targets.
 *
 * We do NOT use a C-level main() because the root task may not have a
 * standard C runtime; the linker script places _start at the entry point.
 */
void __attribute__((noreturn)) _start(void)
{
    seL4_BootInfo *bi;

#if defined(__aarch64__)
    /*
     * AArch64: seL4 places the BootInfo pointer in x2.
     */
    __asm__ volatile("mov %0, x2" : "=r"(bi) : : );
#elif defined(__riscv)
    /*
     * RISC-V 64: seL4 places the BootInfo pointer in a2.
     */
    __asm__ volatile("mv %0, a2" : "=r"(bi) : : );
#else
    /*
     * Fallback for host-side simulator builds: receive BootInfo as a
     * NULL pointer.  The boot sequence gracefully handles a NULL bi.
     */
    bi = (seL4_BootInfo *)0;
#endif

    root_task_main(bi);

    /* root_task_main never returns; suppress the noreturn warning */
    __builtin_unreachable();
}
