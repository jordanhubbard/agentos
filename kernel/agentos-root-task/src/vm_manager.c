/*
 * vm_manager.c — Multi-VM lifecycle manager for agentOS
 * Copyright 2026, agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Passive PD (priority 145) that manages up to 4 simultaneous guest VMs.
 * Controller PPCs in via CH_VM_MANAGER (id=45).
 *
 * Opcodes (in MR0):
 *   OP_VM_CREATE  0x10  MR1=label_vaddr MR2=ram_mb → MR0=ok MR1=slot_id
 *   OP_VM_DESTROY 0x11  MR1=slot_id → MR0=ok
 *   OP_VM_START   0x12  MR1=slot_id → MR0=ok
 *   OP_VM_STOP    0x13  MR1=slot_id → MR0=ok
 *   OP_VM_PAUSE   0x14  MR1=slot_id → MR0=ok
 *   OP_VM_RESUME  0x15  MR1=slot_id → MR0=ok
 *   OP_VM_CONSOLE 0x16  MR1=slot_id → MR0=ok
 *   OP_VM_INFO    0x17  MR1=slot_id → MR0=ok MR1=state MR2=ram_vaddr
 *   OP_VM_LIST    0x18  → MR0=ok MR1=count; vm_list_shmem has vm_list_entry_t[]
 *   OP_VM_SNAPSHOT 0x19 MR1=slot_id → MR0=ok MR1=snap_hash_lo MR2=snap_hash_hi
 *   OP_VM_RESTORE  0x1A MR1=slot_id MR2=snap_lo MR3=snap_hi → MR0=ok
 *
 * This PD wraps the vmm_mux multi-slot multiplexer (kernel/freebsd-vmm/vmm_mux.c)
 * and exposes a clean IPC API to the controller.
 *
 * Phase 1 note: vmm_mux depends on libvmm (au-ts/libvmm) which is only available
 * on AArch64 builds with GUEST_OS=freebsd.  On other platforms the IPC dispatch
 * layer still compiles and functions; vmm_mux_create() will fail gracefully
 * (returning 0xFF) if the underlying vCPU infrastructure is absent.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "../../freebsd-vmm/vmm_mux.h"

/* ── Shared memory output region ─────────────────────────────────────────
 * vm_list_shmem (4KB) is mapped rw into this PD and r into controller.
 * Microkit sets vm_list_vaddr to the mapped virtual address at link time.
 */
uintptr_t vm_list_vaddr;   /* set by Microkit linker (setvar_vaddr) */

/* ── VM list entry written into vm_list_shmem on OP_VM_LIST ─────────────
 * Packed so the controller can read it without alignment surprises.
 */
typedef struct __attribute__((packed)) {
    uint8_t  slot_id;
    uint8_t  state;      /* VM_SLOT_* from vmm_mux.h */
    uint8_t  _pad[2];
    uint32_t ram_mb;
    char     label[16];
} vm_list_entry_t;

/* ── Global VM multiplexer ───────────────────────────────────────────────
 * One vm_mux_t manages all four slots.  Initialised in init().
 */
static vm_mux_t g_mux;

/* ── Result codes ────────────────────────────────────────────────────────
 * Returned in MR0 for all operations.
 */
#define VM_OK      0u
#define VM_ERR     1u
#define VM_NOT_IMPL 0xFEu

/* ── Helper: print a small decimal number without libc ──────────────────*/
static void dbg_u8(uint8_t v)
{
    char buf[4];
    buf[0] = '0' + (char)(v / 100 % 10);
    buf[1] = '0' + (char)(v / 10  % 10);
    buf[2] = '0' + (char)(v       % 10);
    buf[3] = '\0';
    /* Trim leading zeros for readability */
    if (buf[0] == '0' && buf[1] == '0') {
        microkit_dbg_puts(buf + 2);
    } else if (buf[0] == '0') {
        microkit_dbg_puts(buf + 1);
    } else {
        microkit_dbg_puts(buf);
    }
}

/* ── init — called once at PD startup ───────────────────────────────────*/

void init(void)
{
    vmm_mux_init(&g_mux);
    microkit_dbg_puts("[vm_manager] init: 4-slot VM multiplexer ready\n");
}

/* ── protected — handles controller PPCs on CH_VM_MANAGER ───────────────*/

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg)
{
    (void)ch;
    (void)msg;

    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {

    /* ── OP_VM_CREATE ──────────────────────────────────────────────────── */
    case OP_VM_CREATE: {
        /*
         * MR1 = label vaddr (caller writes a NUL-terminated string into
         *        vm_list_shmem before the PPC, or passes NULL/0).
         * MR2 = ram_mb (informational for Phase 1 — actual RAM is
         *        fixed by the .system memory region assignments).
         */
        const char *label = (const char *)(uintptr_t)microkit_mr_get(1);
        /* Guard against NULL or obviously bad pointer */
        if (!label || (uintptr_t)label < 0x1000u) {
            label = "vm";
        }
        uint8_t slot_id = vmm_mux_create(&g_mux, label);
        if (slot_id == 0xFF) {
            microkit_dbg_puts("[vm_manager] CREATE failed: no free slots\n");
            microkit_mr_set(0, VM_ERR);
            return microkit_msginfo_new(0, 1);
        }
        microkit_dbg_puts("[vm_manager] CREATE slot=");
        dbg_u8(slot_id);
        microkit_dbg_puts("\n");
        microkit_mr_set(0, VM_OK);
        microkit_mr_set(1, slot_id);
        return microkit_msginfo_new(0, 2);
    }

    /* ── OP_VM_DESTROY ─────────────────────────────────────────────────── */
    case OP_VM_DESTROY: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        int r = vmm_mux_destroy(&g_mux, slot_id);
        microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_START ───────────────────────────────────────────────────── */
    case OP_VM_START: {
        /*
         * Already running after CREATE; OP_VM_START is a resume-from-stopped
         * semantic (alias for vmm_mux_resume for Phase 1).
         */
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        if (slot_id >= VM_MAX_SLOTS ||
            g_mux.slots[slot_id].state == VM_SLOT_FREE) {
            microkit_mr_set(0, VM_ERR);
        } else if (g_mux.slots[slot_id].state == VM_SLOT_RUNNING ||
                   g_mux.slots[slot_id].state == VM_SLOT_BOOTING) {
            /* Already running — idempotent success */
            microkit_mr_set(0, VM_OK);
        } else {
            int r = vmm_mux_resume(&g_mux, slot_id);
            microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        }
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_STOP ────────────────────────────────────────────────────── */
    case OP_VM_STOP: {
        /*
         * Phase 1: treat stop as pause (not full destroy).
         * A stopped slot stays allocated but its vCPU is suspended.
         */
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        if (slot_id >= VM_MAX_SLOTS ||
            g_mux.slots[slot_id].state == VM_SLOT_FREE) {
            microkit_mr_set(0, VM_ERR);
        } else {
            int r = vmm_mux_pause(&g_mux, slot_id);
            microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        }
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_PAUSE ───────────────────────────────────────────────────── */
    case OP_VM_PAUSE: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        int r = vmm_mux_pause(&g_mux, slot_id);
        microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_RESUME ──────────────────────────────────────────────────── */
    case OP_VM_RESUME: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        int r = vmm_mux_resume(&g_mux, slot_id);
        microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_CONSOLE ─────────────────────────────────────────────────── */
    case OP_VM_CONSOLE: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        int r = vmm_mux_switch(&g_mux, slot_id);
        microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_VM_INFO ────────────────────────────────────────────────────── */
    case OP_VM_INFO: {
        uint8_t slot_id = (uint8_t)microkit_mr_get(1);
        if (slot_id >= VM_MAX_SLOTS) {
            microkit_mr_set(0, VM_ERR);
            return microkit_msginfo_new(0, 1);
        }
        vm_slot_t *s = &g_mux.slots[slot_id];
        microkit_mr_set(0, VM_OK);
        microkit_mr_set(1, (uint32_t)s->state);
        /* Only the low 32 bits of ram_vaddr fit in a single MR on 32-bit ABI */
        microkit_mr_set(2, (uint32_t)(s->ram_vaddr & 0xFFFFFFFFu));
        return microkit_msginfo_new(0, 3);
    }

    /* ── OP_VM_LIST ────────────────────────────────────────────────────── */
    case OP_VM_LIST: {
        uint32_t count = 0;
        if (vm_list_vaddr) {
            vm_list_entry_t *entries =
                (vm_list_entry_t *)(uintptr_t)vm_list_vaddr;
            for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
                vm_slot_t *s = &g_mux.slots[i];
                if (s->state != VM_SLOT_FREE) {
                    entries[count].slot_id = i;
                    entries[count].state   = (uint8_t)s->state;
                    entries[count]._pad[0] = 0;
                    entries[count]._pad[1] = 0;
                    entries[count].ram_mb  = (uint32_t)(s->ram_size >> 20);
                    /* Copy label (NUL-terminated, max 15 chars + NUL) */
                    for (int j = 0; j < 15; j++) {
                        entries[count].label[j] = s->label[j];
                        if (!s->label[j]) break;
                    }
                    entries[count].label[15] = '\0';
                    count++;
                }
            }
        }
        microkit_mr_set(0, VM_OK);
        microkit_mr_set(1, count);
        return microkit_msginfo_new(0, 2);
    }

    /* ── OP_VM_SNAPSHOT / OP_VM_RESTORE ───────────────────────────────── */
    case OP_VM_SNAPSHOT:
    case OP_VM_RESTORE:
        /*
         * Delegated to vm_snapshot PD (Phase 2).
         * Return NOT_IMPL so the controller can fall back gracefully.
         */
        microkit_dbg_puts("[vm_manager] SNAPSHOT/RESTORE: not implemented (Phase 1)\n");
        microkit_mr_set(0, VM_NOT_IMPL);
        return microkit_msginfo_new(0, 1);

    default:
        microkit_dbg_puts("[vm_manager] unknown opcode\n");
        microkit_mr_set(0, VM_ERR);
        return microkit_msginfo_new(0, 1);
    }
}

/* ── notified — no async notifications expected ──────────────────────────*/

void notified(microkit_channel ch)
{
    (void)ch;
    microkit_dbg_puts("[vm_manager] unexpected notification\n");
}
