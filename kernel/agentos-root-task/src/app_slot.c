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

#include <microkit.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spawn.h"

/* Microkit fills this from the system description <map> element */
uintptr_t spawn_elf_shmem_vaddr;

/* console_rings_vaddr — weak fallback so log.c compiles without mapping */
uintptr_t console_rings_vaddr;

/* Channel: channel 0 is spawn_server (notify both ways) */
#define CH_SPAWN  0

/* ── minimal debug output ────────────────────────────────────────────────── */
static void dbg(const char *s) { microkit_dbg_puts(s); }

static void dbg_u32(uint32_t v)
{
    if (v == 0) { microkit_dbg_puts("0"); return; }
    char buf[12]; int i = 0;
    while (v > 0) { buf[i++] = '0' + (char)(v % 10); v /= 10; }
    for (int j = i - 1; j >= 0; j--) {
        char c[2] = { buf[j], '\0' };
        microkit_dbg_puts(c);
    }
}

static void log_launch(const spawn_header_t *hdr)
{
    dbg("[app_slot] launching app '");
    for (int i = 0; i < 32 && hdr->name[i]; i++) {
        char c[2] = { (char)hdr->name[i], '\0' };
        microkit_dbg_puts(c);
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
        microkit_dbg_puts(c);
    }
    dbg("\n");
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    dbg("[app_slot] ready, waiting for ELF staging notification\n");
}

void notified(microkit_channel ch)
{
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
        return;
    }

    /* Log the launch */
    log_launch((const spawn_header_t *)hdr);

    /*
     * MVP: notify SpawnServer immediately — we are "running".
     * A real implementation would parse the ELF, set up an execution
     * environment, and only notify after the entry point is reached.
     */
    microkit_notify(CH_SPAWN);
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    /* app_slot does not serve PPCs in the current design */
    (void)ch;
    (void)msginfo;
    return microkit_msginfo_new(0, 0);
}
