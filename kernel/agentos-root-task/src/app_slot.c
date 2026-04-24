/*
 * app_slot.c — Generic Application Slot Protection Domain
 *
 * All four pre-allocated app slot PDs (app_slot_0..3) run this binary.
 * SpawnServer stages an ELF in spawn_elf_shmem, writes a spawn_header_t
 * at offset 0 of the region, then sends a microkit_notify() to the
 * appropriate slot.  The slot reads the header, logs the app name, and
 * notifies SpawnServer back to confirm it is "running".
 *
 * MVP: ELF execution is stubbed.  The slot acknowledges immediately upon
 * receiving the notification.  Real execution requires a dynamic ELF loader,
 * which is deferred to a future milestone.
 *
 * Priority: 70 (below all system services, above idle).
 * Console slot: 20 (slots 0..3 share this, differentiated by app_id).
 *
 * Channel layout (from this PD's perspective):
 *   id=0: spawn_server — bidirectional notify
 *             spawn_server → app_slot : ELF staged, please start
 *             app_slot → spawn_server : running confirmation
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spawn.h"
#include "sha256_mini.h"
#include "contracts/app_slot_contract.h"

/* Microkit fills this from the system description <map> element */
uintptr_t spawn_elf_shmem_vaddr;

/* log_drain_rings_vaddr — weak fallback so log.c compiles without mapping */
uintptr_t log_drain_rings_vaddr;

/* Channel: channel 0 is spawn_server (notify both ways) */
#define CH_SPAWN  0
/* Channel: channel 1 is exec_server (notify: exec_server signals slot to load) */
#define CH_EXEC   1

/* Slot failure: if ELF hash verification fails we enter this state
 * and do NOT notify SpawnServer — the slot stalls until recycled. */
static bool slot_failed = false;

/* ── minimal debug output ────────────────────────────────────────────────── */
static void dbg(const char *s) { sel4_dbg_puts(s); }

static void dbg_u32(uint32_t v)
{
    if (v == 0) { sel4_dbg_puts("0"); return; }
    char buf[12]; int i = 0;
    while (v > 0) { buf[i++] = '0' + (char)(v % 10); v /= 10; }
    for (int j = i - 1; j >= 0; j--) {
        char c[2] = { buf[j], '\0' };
        sel4_dbg_puts(c);
    }
}

static void log_launch(const spawn_header_t *hdr)
{
    dbg("[app_slot] launching app '");
    for (int i = 0; i < 32 && hdr->name[i]; i++) {
        char c[2] = { (char)hdr->name[i], '\0' };
        sel4_dbg_puts(c);
    }
    dbg("' app_id=");
    dbg_u32(hdr->app_id);
    dbg(" elf_size=");
    dbg_u32(hdr->elf_size);
    dbg(" caps=0x");
    /* hex print of cap_classes */
    static const char hex[] = "0123456789ABCDEF";
    char hbuf[10]; int hi = 0;
    uint32_t cv = hdr->cap_classes;
    do { hbuf[hi++] = hex[cv & 0xF]; cv >>= 4; } while (cv);
    for (int j = hi - 1; j >= 0; j--) {
        char c[2] = { hbuf[j], '\0' };
        sel4_dbg_puts(c);
    }
    dbg("\n");
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

static void app_slot_pd_init(void)
{
    dbg("[app_slot] ready, waiting for ELF staging notification\n");
}

static void app_slot_pd_notified(uint32_t ch)
{
    /*
     * exec_server fires CH_EXEC (id=1) to signal that an ELF has been
     * staged in exec_shmem.  Validate the ELF magic and transition the
     * slot to RUNNING.  In production, seL4_TCB_WriteRegisters + Resume
     * would follow the magic check.
     */
    if (ch == CH_EXEC) {
        if (spawn_elf_shmem_vaddr) {
            uint8_t *elf = (uint8_t *)spawn_elf_shmem_vaddr;
            if (elf[0] == 0x7Fu && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F') {
                /* Valid ELF: set slot state to RUNNING */
                sel4_dbg_puts("[app_slot] ELF magic validated — slot running\n");
                /* Notify exec_server that we're ready */
                /* In production: seL4_TCB_WriteRegisters + Resume here */
                sel4_dbg_puts("[E5-S8] notify-stub
");
            } else {
                sel4_dbg_puts("[app_slot] not an ELF — ignored\n");
            }
        }
        return;
    }

    if (ch != CH_SPAWN) return;

    if (!spawn_elf_shmem_vaddr) {
        dbg("[app_slot] error: spawn_elf_shmem not mapped\n");
        return;
    }

    const volatile spawn_header_t *hdr =
        (const volatile spawn_header_t *)spawn_elf_shmem_vaddr;

    if (hdr->magic != SPAWN_MAGIC) {
        dbg("[app_slot] error: bad magic in spawn_elf_shmem (not SPWN)\n");
        return;
    }

    if (hdr->elf_size == 0 || hdr->elf_size > SPAWN_MAX_ELF_SIZE) {
        dbg("[app_slot] error: invalid elf_size in spawn header\n");
        slot_failed = true;
        return;
    }

    /* Log the launch */
    log_launch((const spawn_header_t *)hdr);

    /*
     * ELF hash verification: compute SHA-256 of the ELF image bytes and
     * compare against the hash written by SpawnServer into the header.
     * If the hashes differ the image has been tampered with or corrupted;
     * enter SLOT_FAILED state and do NOT notify SpawnServer.
     */
    const uint8_t *elf_bytes =
        (const uint8_t *)(spawn_elf_shmem_vaddr + SPAWN_HEADER_SIZE);
    uint8_t computed[32];
    sha256_mini(elf_bytes, hdr->elf_size, computed);

    bool hash_ok = true;
    for (int i = 0; i < 32; i++) {
        if (computed[i] != hdr->elf_sha256[i]) {
            hash_ok = false;
            break;
        }
    }

    if (!hash_ok) {
        dbg("[app_slot] SECURITY: ELF SHA-256 mismatch — aborting load\n");
        slot_failed = true;
        return;
    }

    slot_failed = false;

    /*
     * MVP: notify SpawnServer — we are "running".
     * A real implementation would parse the ELF, set up an execution
     * environment, and only notify after the entry point is reached.
     */
    sel4_dbg_puts("[E5-S8] notify-stub
");
}

static uint32_t app_slot_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx)
{
    /* app_slot does not serve PPCs in the current design */
    (void)b;
    (void)ctx;
    rep->length = 0;
        return SEL4_ERR_OK;
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void app_slot_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    app_slot_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, app_slot_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
