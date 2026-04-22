/*
 * agentOS SpawnServer Protection Domain
 *
 * SpawnServer dynamically launches application PDs by assigning pre-allocated
 * "app slots" (app_slot_0..3) and loading ELF images into them via shared
 * memory.  The lifecycle is:
 *
 *   1. Caller writes app name + ELF path into spawn_config_shmem, then PPCs
 *      OP_SPAWN_LAUNCH (with cap_classes in MR1).
 *   2. SpawnServer finds a free slot, opens the ELF from VFS, streams it into
 *      spawn_elf_shmem+64, writes a spawn_header_t at byte 0, then
 *      microkit_notify()s the slot channel.
 *   3. The pre-compiled app-loader ELF in the slot wakes up, loads the image,
 *      and sends a completion notification back.
 *   4. SpawnServer sets the slot state to RUNNING and the app is live.
 *
 * Kill:  state → KILLED; slot channel notified (slot resets itself).
 * Status/List: read-only queries answered directly from the slot table.
 *
 * Priority: 120 (ACTIVE — runs independently, not purely passive).
 *
 * Channel map (SpawnServer perspective):
 *   CH0  = controller    (pp=true inbound)
 *   CH1  = init_agent    (pp=true inbound)
 *   CH2  = app_manager   (pp=true inbound)
 *   CH3  = vfs_server    (SpawnServer PPCs OUT)
 *   CH4  = app_slot_0    (SpawnServer notifies OUT / receives completion back)
 *   CH5  = app_slot_1
 *   CH6  = app_slot_2
 *   CH7  = app_slot_3
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "spawn.h"
#include "vfs.h"
#include "sha256_mini.h"

/* ── Shared memory base addresses (set by Microkit setvar_vaddr) ─────────── */
uintptr_t spawn_elf_shmem_vaddr;          /* 512 KB staging: SpawnServer at 0x5000000 */
uintptr_t spawn_config_shmem_vaddr;       /* 4 KB config:    SpawnServer at 0x6000000 */
uintptr_t vfs_io_shmem_vaddr; /* VFS shmem:      SpawnServer at 0x7000000 */
uintptr_t log_drain_rings_vaddr;      /* log_drain ring (required by log_drain_write) */

/* ── Slot table ──────────────────────────────────────────────────────────── */
typedef struct {
    bool             active;
    uint32_t         app_id;
    uint32_t         state;          /* SPAWN_SLOT_* */
    uint32_t         cap_classes;
    char             name[32];
    uint64_t         launch_tick;
    microkit_channel slot_ch;        /* CH4..CH7 */
} spawn_slot_t;

static spawn_slot_t slots[SPAWN_MAX_SLOTS];
static uint32_t     next_app_id = 1;
static uint64_t     boot_tick   = 0;

/* ── Monotonic tick counter (incremented on each protected() call) ───────── */
static uint64_t tick_counter = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Bounded string copy — always null-terminates dst[0..n-1] */
static void spawn_strlcpy(char *dst, const char *src, uint32_t n) {
    uint32_t i = 0;
    for (; i < n - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* Write a uint32_t as 8 hex digits into buf (no null terminator) */
static void u32_to_hex(uint32_t v, char buf[8]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        buf[i] = h[v & 0xF];
        v >>= 4;
    }
}

/* Print a uint32 as decimal into a fixed buffer, return length */
static int u32_to_dec(uint32_t v, char buf[12]) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[12];
    int  t = 0;
    while (v > 0 && t < 11) { tmp[t++] = '0' + (v % 10); v /= 10; }
    int len = t;
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

/* Find a free slot index, or -1 if none available */
static int find_free_slot(void) {
    for (int i = 0; i < SPAWN_MAX_SLOTS; i++) {
        if (!slots[i].active) return i;
    }
    return -1;
}

/* Find slot index by app_id, or -1 if not found */
static int find_slot_by_app_id(uint32_t app_id) {
    for (int i = 0; i < SPAWN_MAX_SLOTS; i++) {
        if (slots[i].active && slots[i].app_id == app_id) return i;
    }
    return -1;
}

/* Count free slots */
static uint32_t count_free_slots(void) {
    uint32_t n = 0;
    for (int i = 0; i < SPAWN_MAX_SLOTS; i++) {
        if (!slots[i].active) n++;
    }
    return n;
}

/* ── VFS helpers ─────────────────────────────────────────────────────────── */

/*
 * vfs_open: write path into vfs shmem, call OP_VFS_OPEN, return handle.
 * Returns 0xFFFFFFFF on failure (VFS returns non-zero MR0 or handle == 0).
 */
static uint32_t vfs_open(const char *path) {
    /* Write null-terminated path into vfs_io_shmem at the path offset */
    volatile uint8_t *shmem = (volatile uint8_t *)vfs_io_shmem_vaddr;
    volatile uint8_t *path_buf = shmem + VFS_SHMEM_PATH_OFF;
    uint32_t i = 0;
    for (; i < VFS_SHMEM_PATH_MAX - 1 && path[i]; i++) {
        path_buf[i] = (uint8_t)path[i];
    }
    path_buf[i] = 0;

    /* Memory barrier before PPC */
    __asm__ volatile("" ::: "memory");

    microkit_mr_set(0, OP_VFS_OPEN);
    microkit_mr_set(1, VFS_O_RD);
    microkit_msginfo reply = microkit_ppcall(SPAWN_CH_VFS,
                                 microkit_msginfo_new(OP_VFS_OPEN, 2));
    (void)reply;

    uint32_t result = (uint32_t)microkit_mr_get(0);
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    if (result != VFS_OK || handle == 0) {
        return 0xFFFFFFFFu;
    }
    return handle;
}

/*
 * vfs_stat: call OP_VFS_STAT on an open handle, return file size (lo 32 bits).
 * Returns 0xFFFFFFFF on failure.
 */
static uint32_t vfs_stat(uint32_t handle) {
    microkit_mr_set(0, OP_VFS_STAT);
    microkit_mr_set(1, handle);
    microkit_msginfo reply = microkit_ppcall(SPAWN_CH_VFS,
                                 microkit_msginfo_new(OP_VFS_STAT, 2));
    (void)reply;

    uint32_t result = (uint32_t)microkit_mr_get(0);
    if (result != VFS_OK) return 0xFFFFFFFFu;

    /* MR1=size_lo, MR2=size_hi — we only support 32-bit sizes for now */
    uint32_t size_hi = (uint32_t)microkit_mr_get(2);
    if (size_hi != 0) return 0xFFFFFFFFu;  /* file too large */
    return (uint32_t)microkit_mr_get(1);
}

/*
 * vfs_close: close an open handle.
 */
static void vfs_close(uint32_t handle) {
    microkit_mr_set(0, OP_VFS_CLOSE);
    microkit_mr_set(1, handle);
    microkit_ppcall(SPAWN_CH_VFS, microkit_msginfo_new(OP_VFS_CLOSE, 2));
}

/*
 * vfs_read_all: read up to elf_size bytes from handle into dst, using the VFS
 * shmem data area (vfs_io_shmem+816) as the intermediate buffer.
 *
 * Returns the total number of bytes actually read, or 0xFFFFFFFF on error.
 */
static uint32_t vfs_read_all(uint32_t handle, uint8_t *dst, uint32_t elf_size) {
    const uint32_t chunk_max = VFS_SHMEM_DATA_MAX;
    uint32_t       offset    = 0;
    volatile uint8_t *data_buf =
        (volatile uint8_t *)(vfs_io_shmem_vaddr + VFS_SHMEM_DATA_OFF);

    while (offset < elf_size) {
        uint32_t remaining = elf_size - offset;
        uint32_t want      = remaining < chunk_max ? remaining : chunk_max;

        microkit_mr_set(0, OP_VFS_READ);
        microkit_mr_set(1, handle);
        microkit_mr_set(2, offset);       /* offset_lo */
        microkit_mr_set(3, 0);            /* offset_hi */
        microkit_mr_set(4, want);
        microkit_msginfo reply = microkit_ppcall(SPAWN_CH_VFS,
                                     microkit_msginfo_new(OP_VFS_READ, 5));
        (void)reply;

        uint32_t result     = (uint32_t)microkit_mr_get(0);
        uint32_t bytes_read = (uint32_t)microkit_mr_get(1);

        if (result != VFS_OK || bytes_read == 0) {
            return 0xFFFFFFFFu;
        }

        /* Memory barrier: ensure VFS data is visible before we copy */
        __asm__ volatile("" ::: "memory");

        /* Copy from VFS shmem data area into ELF staging destination */
        for (uint32_t i = 0; i < bytes_read; i++) {
            dst[offset + i] = data_buf[i];
        }

        offset += bytes_read;

        /* If server gave us fewer bytes than requested we may be at EOF */
        if (bytes_read < want) break;
    }

    return offset;
}

/* ── OP_SPAWN_LAUNCH ─────────────────────────────────────────────────────── */

static microkit_msginfo handle_launch(void) {
    uint32_t cap_classes = (uint32_t)microkit_mr_get(1);
    /* MR2 = flags (reserved, not used in v1) */

    /* Read name and elf_path from spawn_config_shmem */
    volatile uint8_t *cfg = (volatile uint8_t *)spawn_config_shmem_vaddr;

    char name[SPAWN_CONFIG_NAME_MAX];
    char elf_path[SPAWN_CONFIG_PATH_MAX];

    for (uint32_t i = 0; i < SPAWN_CONFIG_NAME_MAX; i++) {
        name[i] = (char)cfg[SPAWN_CONFIG_NAME_OFF + i];
    }
    name[SPAWN_CONFIG_NAME_MAX - 1] = '\0';

    for (uint32_t i = 0; i < SPAWN_CONFIG_PATH_MAX; i++) {
        elf_path[i] = (char)cfg[SPAWN_CONFIG_PATH_OFF + i];
    }
    elf_path[SPAWN_CONFIG_PATH_MAX - 1] = '\0';

    /* Validate inputs */
    if (name[0] == '\0' || elf_path[0] == '\0') {
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                    "[spawn_server] LAUNCH REJECT: empty name or path\n");
        microkit_mr_set(0, SPAWN_ERR_INVAL);
        return microkit_msginfo_new(0, 1);
    }

    /* Find a free slot */
    int si = find_free_slot();
    if (si < 0) {
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                    "[spawn_server] LAUNCH REJECT: no free slots\n");
        microkit_mr_set(0, SPAWN_ERR_NO_SLOTS);
        return microkit_msginfo_new(0, 1);
    }

    /* Log the launch attempt */
    log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, "[spawn_server] LAUNCH '");
    log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, name);
    log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, "' from '");
    log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, elf_path);
    log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, "'\n");

    /* Open ELF via VFS */
    uint32_t vfs_handle = vfs_open(elf_path);
    if (vfs_handle == 0xFFFFFFFFu) {
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                    "[spawn_server] LAUNCH FAIL: VFS open failed\n");
        microkit_mr_set(0, SPAWN_ERR_VFS);
        return microkit_msginfo_new(0, 1);
    }

    /* Stat to get ELF size */
    uint32_t elf_size = vfs_stat(vfs_handle);
    if (elf_size == 0xFFFFFFFFu) {
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                    "[spawn_server] LAUNCH FAIL: VFS stat failed\n");
        vfs_close(vfs_handle);
        microkit_mr_set(0, SPAWN_ERR_VFS);
        return microkit_msginfo_new(0, 1);
    }

    /* Validate size: must fit after the 64-byte launch header */
    if (elf_size == 0 || elf_size > SPAWN_MAX_ELF_SIZE) {
        char dbuf[12];
        u32_to_dec(elf_size, dbuf);
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                    "[spawn_server] LAUNCH FAIL: ELF too large (");
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, dbuf);
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, " bytes)\n");
        vfs_close(vfs_handle);
        microkit_mr_set(0, SPAWN_ERR_TOO_LARGE);
        return microkit_msginfo_new(0, 1);
    }

    /* Read ELF into spawn_elf_shmem starting at byte SPAWN_HEADER_SIZE */
    uint8_t *elf_dst = (uint8_t *)(spawn_elf_shmem_vaddr + SPAWN_HEADER_SIZE);
    uint32_t bytes_read = vfs_read_all(vfs_handle, elf_dst, elf_size);
    vfs_close(vfs_handle);

    if (bytes_read != elf_size) {
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                    "[spawn_server] LAUNCH FAIL: VFS read incomplete\n");
        microkit_mr_set(0, SPAWN_ERR_VFS);
        return microkit_msginfo_new(0, 1);
    }

    /* Assign app_id and fill slot record */
    uint32_t app_id = next_app_id++;
    tick_counter++;

    slots[si].active      = true;
    slots[si].app_id      = app_id;
    slots[si].state       = SPAWN_SLOT_LOADING;
    slots[si].cap_classes = cap_classes;
    slots[si].launch_tick = tick_counter;
    slots[si].slot_ch     = (microkit_channel)(SPAWN_CH_APP_SLOT_0 + si);
    spawn_strlcpy(slots[si].name, name, 32);

    /* Compute SHA-256 of the loaded ELF image */
    uint8_t elf_hash[32];
    sha256_mini(elf_dst, elf_size, elf_hash);

    /* Write spawn_header_t at spawn_elf_shmem[0..95] */
    volatile spawn_header_t *hdr = (volatile spawn_header_t *)spawn_elf_shmem_vaddr;
    hdr->magic       = SPAWN_MAGIC;
    hdr->elf_size    = elf_size;
    hdr->cap_classes = cap_classes;
    hdr->app_id      = app_id;
    hdr->vfs_handle  = 0;
    hdr->net_vnic_id = 0;
    for (uint32_t i = 0; i < 32; i++) {
        hdr->name[i] = (uint8_t)(i < 31 ? name[i] : 0);
    }
    for (uint32_t i = 0; i < 32; i++) {
        hdr->elf_sha256[i] = elf_hash[i];
    }

    /* Memory barrier: header and ELF data must be visible before notify */
    __asm__ volatile("" ::: "memory");

    /* Notify the app slot to wake up and load the ELF */
    microkit_notify(slots[si].slot_ch);

    /* Log success */
    {
        char idbuf[12];
        u32_to_dec(app_id, idbuf);
        char sibuf[4];
        u32_to_dec((uint32_t)si, sibuf);
        char szbuf[12];
        u32_to_dec(elf_size, szbuf);
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                    "[spawn_server] LAUNCHED app_id=");
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, idbuf);
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, " slot=");
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, sibuf);
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, " elf_size=");
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, szbuf);
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, "\n");
    }

    microkit_mr_set(0, SPAWN_OK);
    microkit_mr_set(1, app_id);
    microkit_mr_set(2, (uint64_t)si);
    return microkit_msginfo_new(0, 3);
}

/* ── OP_SPAWN_KILL ───────────────────────────────────────────────────────── */

static microkit_msginfo handle_kill(void) {
    uint32_t app_id = (uint32_t)microkit_mr_get(1);

    int si = find_slot_by_app_id(app_id);
    if (si < 0) {
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                    "[spawn_server] KILL: app_id not found\n");
        microkit_mr_set(0, SPAWN_ERR_NOT_FOUND);
        return microkit_msginfo_new(0, 1);
    }

    /* Log the kill */
    {
        char idbuf[12];
        u32_to_dec(app_id, idbuf);
        char sibuf[4];
        u32_to_dec((uint32_t)si, sibuf);
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                    "[spawn_server] KILL app_id=");
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, idbuf);
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, " slot=");
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, sibuf);
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, "\n");
    }

    slots[si].state = SPAWN_SLOT_KILLED;

    /*
     * Signal the slot to reset.  The slot interprets a notification while
     * RUNNING as a teardown request and will idle-wait again.
     */
    microkit_notify(slots[si].slot_ch);

    /*
     * Mark slot as free immediately — the slot PD is self-resetting.
     * The app_id is retired; a future launch will allocate a fresh one.
     */
    slots[si].active = false;

    microkit_mr_set(0, SPAWN_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── OP_SPAWN_STATUS ─────────────────────────────────────────────────────── */

static microkit_msginfo handle_status(void) {
    uint32_t app_id = (uint32_t)microkit_mr_get(1);

    if (app_id == 0xFFFFFFFFu) {
        /* Aggregate: return total active slots and free slots */
        uint32_t free_cnt = count_free_slots();
        microkit_mr_set(0, SPAWN_OK);
        microkit_mr_set(1, (uint64_t)(SPAWN_MAX_SLOTS - free_cnt));
        microkit_mr_set(2, 0);
        return microkit_msginfo_new(0, 3);
    }

    int si = find_slot_by_app_id(app_id);
    if (si < 0) {
        microkit_mr_set(0, SPAWN_ERR_NOT_FOUND);
        return microkit_msginfo_new(0, 1);
    }

    microkit_mr_set(0, SPAWN_OK);
    microkit_mr_set(1, slots[si].state);
    microkit_mr_set(2, slots[si].cap_classes);
    return microkit_msginfo_new(0, 3);
}

/* ── OP_SPAWN_LIST ───────────────────────────────────────────────────────── */

static microkit_msginfo handle_list(void) {
    volatile uint8_t *cfg = (volatile uint8_t *)spawn_config_shmem_vaddr;
    volatile spawn_slot_info_t *list =
        (volatile spawn_slot_info_t *)(cfg + SPAWN_CONFIG_LIST_OFF);

    uint32_t count = 0;
    for (int i = 0; i < SPAWN_MAX_SLOTS && count < SPAWN_LIST_MAX_ENTRIES; i++) {
        if (!slots[i].active) continue;
        list[count].app_id      = slots[i].app_id;
        list[count].slot_index  = (uint32_t)i;
        list[count].state       = slots[i].state;
        list[count].cap_classes = slots[i].cap_classes;
        list[count].launch_tick = slots[i].launch_tick;
        for (uint32_t c = 0; c < 32; c++) {
            list[count].name[c] = slots[i].name[c];
        }
        count++;
    }

    /* Memory barrier: list data visible before caller reads it */
    __asm__ volatile("" ::: "memory");

    microkit_mr_set(0, SPAWN_OK);
    microkit_mr_set(1, count);
    return microkit_msginfo_new(0, 2);
}

/* ── OP_SPAWN_HEALTH ─────────────────────────────────────────────────────── */

static microkit_msginfo handle_health(void) {
    microkit_mr_set(0, SPAWN_OK);
    microkit_mr_set(1, count_free_slots());
    microkit_mr_set(2, SPAWN_VERSION);
    return microkit_msginfo_new(0, 3);
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void) {
    agentos_log_boot("spawn_server");
    log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                "[spawn_server] SpawnServer PD starting...\n");

    /* Initialise slot table */
    for (int i = 0; i < SPAWN_MAX_SLOTS; i++) {
        slots[i].active      = false;
        slots[i].app_id      = 0;
        slots[i].state       = SPAWN_SLOT_FREE;
        slots[i].cap_classes = 0;
        slots[i].launch_tick = 0;
        slots[i].slot_ch     = (microkit_channel)(SPAWN_CH_APP_SLOT_0 + i);
        for (int c = 0; c < 32; c++) slots[i].name[c] = '\0';
    }

    boot_tick    = 0;
    next_app_id  = 1;
    tick_counter = 0;

    {
        char fbuf[4];
        u32_to_dec(SPAWN_MAX_SLOTS, fbuf);
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                    "[spawn_server] App slots available: ");
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, fbuf);
        log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, "\n");
    }

    log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                "[spawn_server] *** SpawnServer ALIVE — accepting launch requests ***\n");
}

/*
 * notified(): receive async notifications.
 *
 * Channels CH4..CH7 are the app slots.  When a slot finishes loading the ELF
 * it sends a notification back here; we advance the slot state to RUNNING.
 *
 * Any other channel notification is informational only.
 */
void notified(microkit_channel ch) {
    if (ch >= SPAWN_CH_APP_SLOT_0 &&
        ch <= SPAWN_CH_APP_SLOT_0 + SPAWN_MAX_SLOTS - 1) {
        int si = ch - SPAWN_CH_APP_SLOT_0;
        if (slots[si].active && slots[si].state == SPAWN_SLOT_LOADING) {
            slots[si].state = SPAWN_SLOT_RUNNING;
            {
                char idbuf[12];
                u32_to_dec(slots[si].app_id, idbuf);
                char sibuf[4];
                u32_to_dec((uint32_t)si, sibuf);
                log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                            "[spawn_server] Slot ");
                log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, sibuf);
                log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                            " load complete — app_id=");
                log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, idbuf);
                log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, " RUNNING\n");
            }
        }
        return;
    }

    /* Unexpected notification channel — log and ignore */
    agentos_log_channel("spawn_server", ch);
}

/*
 * protected(): dispatch inbound PPC calls from controller, init_agent, or
 * app_manager.  The operation code is in MR0.
 */
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)msginfo;  /* label unused; op is in MR0 */

    tick_counter++;

    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
        case OP_SPAWN_LAUNCH:  return handle_launch();
        case OP_SPAWN_KILL:    return handle_kill();
        case OP_SPAWN_STATUS:  return handle_status();
        case OP_SPAWN_LIST:    return handle_list();
        case OP_SPAWN_HEALTH:  return handle_health();

        default:
            log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID,
                        "[spawn_server] Unknown op on ch=");
            {
                char chbuf[4];
                u32_to_dec((uint32_t)ch, chbuf);
                log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, chbuf);
            }
            log_drain_write(SPAWN_CONSOLE_SLOT, SPAWN_PD_ID, "\n");
            microkit_mr_set(0, SPAWN_ERR_INVAL);
            return microkit_msginfo_new(0, 1);
    }
}
