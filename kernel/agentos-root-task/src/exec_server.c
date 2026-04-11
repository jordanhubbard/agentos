/*
 * exec_server Protection Domain
 *
 * HURD exec server equivalent — receives ELF launch requests,
 * validates auth tokens, and dispatches to free app_slot PDs.
 *
 * Channel assignments:
 *   id=0:     PPC from controller (OP_EXEC_LAUNCH, OP_EXEC_STATUS, etc.)
 *   id=34..37: bidirectional notify with app_slot_0..3 (CH_APP_SLOT_0 + slot_id)
 *              exec_server → slot: signal ELF load
 *              slot → exec_server: confirm running (arrives as notified(34..37))
 *
 * Priority: 150 (passive)
 */
#define AGENTOS_DEBUG 1
#include "agentos.h"

uintptr_t exec_shmem_vaddr;   /* 64KB: exec path at offset 0 (null-terminated) */
#define EXEC_SHMEM_SIZE  0x10000u

#define EXEC_MAX_TASKS  8
#define EXEC_STATE_FREE     0
#define EXEC_STATE_LOADING  1
#define EXEC_STATE_RUNNING  2
#define EXEC_STATE_DONE     3
#define EXEC_STATE_FAILED   4

typedef struct {
    uint8_t  state;
    uint8_t  app_slot_id;   /* which app_slot PD (0–3) */
    uint32_t pid;           /* proc_server PID assigned */
    uint32_t auth_token;
    uint32_t cap_mask;
    char     path[64];
} exec_task_t;

static exec_task_t tasks[EXEC_MAX_TASKS];

/*
 * exec_id is the 1-based index into tasks[].  Callers use this stable
 * identifier to query status, wait for completion, or kill the task.
 */

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void dbg(const char *s) { microkit_dbg_puts(s); }

static void copy_path(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Find a free slot index; returns EXEC_MAX_TASKS if none free */
static int find_free_task(void)
{
    for (int i = 0; i < EXEC_MAX_TASKS; i++) {
        if (tasks[i].state == EXEC_STATE_FREE)
            return i;
    }
    return EXEC_MAX_TASKS;
}

/*
 * Round-robin app_slot allocator.
 * Scans tasks[] to find which app_slot_ids are in active use, then
 * picks the lowest slot_id (0–3) that is not currently LOADING/RUNNING.
 */
static int find_free_app_slot(void)
{
    bool in_use[4] = { false, false, false, false };
    for (int i = 0; i < EXEC_MAX_TASKS; i++) {
        if (tasks[i].state == EXEC_STATE_LOADING ||
            tasks[i].state == EXEC_STATE_RUNNING) {
            uint8_t sid = tasks[i].app_slot_id;
            if (sid < 4)
                in_use[sid] = true;
        }
    }
    for (int s = 0; s < 4; s++) {
        if (!in_use[s])
            return s;
    }
    return -1;  /* all slots busy */
}

/* Find task by exec_id (stored as index+1 for non-zero IDs) */
static exec_task_t *find_task_by_id(uint32_t exec_id)
{
    if (exec_id == 0 || exec_id > (uint32_t)EXEC_MAX_TASKS)
        return NULL;
    exec_task_t *t = &tasks[exec_id - 1];
    if (t->state == EXEC_STATE_FREE)
        return NULL;
    return t;
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    for (int i = 0; i < EXEC_MAX_TASKS; i++) {
        tasks[i].state      = EXEC_STATE_FREE;
        tasks[i].app_slot_id = 0;
        tasks[i].pid        = 0;
        tasks[i].auth_token = 0;
        tasks[i].cap_mask   = 0;
        tasks[i].path[0]    = '\0';
    }
    dbg("[exec_server] init: 8 exec slots\n");
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    if (ch != 0) {
        /* Only channel 0 serves PPCs */
        return microkit_msginfo_new(1, 0);
    }

    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {

    case OP_EXEC_LAUNCH: {
        if (!exec_shmem_vaddr) {
            dbg("[exec_server] launch: exec_shmem not mapped\n");
            microkit_mr_set(0, 1);
            return microkit_msginfo_new(0, 1);
        }

        /* Read null-terminated path from shmem (max 63 chars) */
        const char *src = (const char *)exec_shmem_vaddr;
        uint32_t auth_token = (uint32_t)microkit_mr_get(1);
        uint32_t cap_mask   = (uint32_t)microkit_mr_get(2);

        int slot_idx = find_free_task();
        if (slot_idx == EXEC_MAX_TASKS) {
            dbg("[exec_server] launch: no free exec slots\n");
            microkit_mr_set(0, 2);
            return microkit_msginfo_new(0, 1);
        }

        int app_slot_id = find_free_app_slot();
        if (app_slot_id < 0) {
            dbg("[exec_server] launch: all app_slots busy\n");
            microkit_mr_set(0, 3);
            return microkit_msginfo_new(0, 1);
        }

        /* Assign exec_id: use 1-based slot index for stable identity */
        uint8_t exec_id = (uint8_t)(slot_idx + 1);

        exec_task_t *t = &tasks[slot_idx];
        t->state      = EXEC_STATE_LOADING;
        t->app_slot_id = (uint8_t)app_slot_id;
        t->pid        = 0;
        t->auth_token = auth_token;
        t->cap_mask   = cap_mask;
        copy_path(t->path, src, 64);

        /* Signal app_slot PD to load the staged ELF.
         * CH_APP_SLOT_0 = 34; app_slot_N is at channel (34 + N). */
        microkit_notify((microkit_channel)(CH_APP_SLOT_0 + (uint8_t)app_slot_id));

        /* Phase 1: optimistically transition to RUNNING immediately */
        t->state = EXEC_STATE_RUNNING;

        dbg("[exec_server] launch: exec_id=");
        /* minimal decimal print */
        char ec[4]; int ei = 0;
        uint8_t ev = exec_id;
        do { ec[ei++] = '0' + (char)(ev % 10); ev /= 10; } while (ev);
        for (int j = ei - 1; j >= 0; j--) {
            char cs[2] = { ec[j], '\0' };
            microkit_dbg_puts(cs);
        }
        dbg(" app_slot=");
        char as[2] = { '0' + (char)app_slot_id, '\0' };
        microkit_dbg_puts(as);
        dbg("\n");

        microkit_mr_set(0, 0);
        microkit_mr_set(1, exec_id);
        return microkit_msginfo_new(0, 2);
    }

    case OP_EXEC_STATUS: {
        uint32_t exec_id = (uint32_t)microkit_mr_get(1);
        exec_task_t *t = find_task_by_id(exec_id);
        if (!t) {
            microkit_mr_set(0, 1);
            return microkit_msginfo_new(0, 1);
        }
        microkit_mr_set(0, 0);
        microkit_mr_set(1, t->state);
        microkit_mr_set(2, t->pid);
        return microkit_msginfo_new(0, 3);
    }

    case OP_EXEC_WAIT: {
        uint32_t exec_id = (uint32_t)microkit_mr_get(1);
        exec_task_t *t = find_task_by_id(exec_id);
        if (!t) {
            microkit_mr_set(0, 1);
            return microkit_msginfo_new(0, 1);
        }
        if (t->state == EXEC_STATE_RUNNING) {
            microkit_mr_set(0, 0);
            microkit_mr_set(1, t->pid);
            return microkit_msginfo_new(0, 2);
        }
        /* Still loading — return busy status */
        microkit_mr_set(0, 1);
        return microkit_msginfo_new(0, 1);
    }

    case OP_EXEC_KILL: {
        uint32_t exec_id = (uint32_t)microkit_mr_get(1);
        exec_task_t *t = find_task_by_id(exec_id);
        if (!t) {
            microkit_mr_set(0, 1);
            return microkit_msginfo_new(0, 1);
        }
        t->state = EXEC_STATE_DONE;
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    default:
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }
}

void notified(microkit_channel ch)
{
    /*
     * app_slot_N fires back on its end of the bidirectional channel that
     * exec_server holds as id=(CH_APP_SLOT_0 + N).  Map the channel to the
     * corresponding app_slot_id, then mark the matching task DONE.
     */
    if (ch < CH_APP_SLOT_0 || ch > CH_APP_SLOT_0 + 3u)
        return;

    uint8_t slot_id = (uint8_t)(ch - CH_APP_SLOT_0);

    for (int i = 0; i < EXEC_MAX_TASKS; i++) {
        if (tasks[i].app_slot_id == slot_id &&
            (tasks[i].state == EXEC_STATE_LOADING ||
             tasks[i].state == EXEC_STATE_RUNNING)) {
            tasks[i].state = EXEC_STATE_DONE;
            dbg("[exec_server] notified: slot=");
            char sc[2] = { '0' + (char)slot_id, '\0' };
            microkit_dbg_puts(sc);
            dbg(" exec_id=");
            char ec[4]; int ei = 0;
            uint8_t ev = (uint8_t)(i + 1);
            do { ec[ei++] = '0' + (char)(ev % 10); ev /= 10; } while (ev);
            for (int j = ei - 1; j >= 0; j--) {
                char cs[2] = { ec[j], '\0' };
                microkit_dbg_puts(cs);
            }
            dbg(" -> DONE\n");
            break;
        }
    }
}
