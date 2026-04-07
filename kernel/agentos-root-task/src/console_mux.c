/*
 * agentOS Console Multiplexer Protection Domain
 *
 * The "tmux for agentOS" — multiplexes per-PD output streams onto the debug
 * UART with attach/detach/switch semantics.
 *
 * Architecture:
 *   - Each PD gets a 4KB output ring in shared memory (console_rings region)
 *   - PDs write log lines to their ring; console_mux drains active rings to UART
 *   - Controller can send commands: attach, detach, switch, list, broadcast
 *   - Active session determines which PD's output goes to UART
 *   - "broadcast" mode shows all PDs (tagged with source name)
 *   - Scrollback: each ring retains last N lines for re-display on attach
 *
 * Ring layout (per PD, 4KB each):
 *   [0..3]     magic (0xC0DE_4D55 = "CODEMUX")
 *   [4..7]     pd_id (which PD owns this ring)
 *   [8..11]    head  (write offset, updated by PD)
 *   [12..15]   tail  (read offset, updated by console_mux)
 *   [16..4095] circular character buffer (4080 bytes)
 *
 * IPC Protocol (controller -> console_mux, channel CH_CONSOLE_MUX):
 *   MR0 = op code:
 *     OP_CONSOLE_ATTACH   (0x80) - MR1=pd_id: switch active view to pd_id
 *     OP_CONSOLE_DETACH   (0x81) - detach (show nothing, or revert to broadcast)
 *     OP_CONSOLE_LIST     (0x82) - return bitmask of active PDs in MR1
 *     OP_CONSOLE_MODE     (0x83) - MR1=mode: 0=single, 1=broadcast, 2=split
 *     OP_CONSOLE_INJECT   (0x84) - MR1..MR4=chars: inject input to active PD
 *     OP_CONSOLE_SCROLL   (0x85) - MR1=lines: scroll back N lines in active session
 *     OP_CONSOLE_STATUS   (0x86) - return: MR1=active_pd, MR2=mode, MR3=ring_count
 *
 * Priority: 160 (above workers/agents, below eventbus — drain promptly)
 * Mode: passive (only runs when notified or PPC'd into)
 *
 * This is the console session multiplexer that gives agentOS its interactive
 * personality. Every agent gets a window. You choose which one you're watching.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* ─── Configuration ───────────────────────────────────────────────────── */

#define MAX_CONSOLE_SESSIONS  16
#define RING_SIZE             4096
#define RING_HEADER_SIZE      16
#define RING_BUF_SIZE         (RING_SIZE - RING_HEADER_SIZE)
#define CONSOLE_RING_MAGIC    0xC0DE4D55

/* Scrollback: we keep last N lines per session for re-display on attach */
#define MAX_SCROLLBACK_LINES  64
#define MAX_LINE_LEN          120

/* ─── Op codes (MR0 in PPC requests) ─────────────────────────────────── */

#define OP_CONSOLE_ATTACH     0x80
#define OP_CONSOLE_DETACH     0x81
#define OP_CONSOLE_LIST       0x82
#define OP_CONSOLE_MODE       0x83
#define OP_CONSOLE_INJECT     0x84
#define OP_CONSOLE_SCROLL     0x85
#define OP_CONSOLE_STATUS     0x86
#define OP_CONSOLE_WRITE      0x87  /* PD -> console_mux: flush ring */

/* ─── Display modes ───────────────────────────────────────────────────── */

#define MODE_SINGLE     0   /* Show only the attached PD */
#define MODE_BROADCAST  1   /* Show all PDs, tagged with source */
#define MODE_SPLIT      2   /* Show up to 4 PDs, labeled */

/* console_ring_header_t is defined in agentos.h */

/* ─── Scrollback line buffer ──────────────────────────────────────────── */

typedef struct {
    char lines[MAX_SCROLLBACK_LINES][MAX_LINE_LEN];
    uint32_t line_head;   /* next write slot (circular) */
    uint32_t line_count;  /* total lines stored (max MAX_SCROLLBACK_LINES) */
} scrollback_t;

/* ─── Session descriptor ──────────────────────────────────────────────── */

typedef struct {
    bool     active;
    uint32_t pd_id;
    char     name[16];     /* human-readable PD name */
    uint64_t bytes_total;  /* total bytes received from this PD */
    uint64_t lines_total;  /* total lines received */
    scrollback_t scrollback;
} console_session_t;

/* ─── State ───────────────────────────────────────────────────────────── */

/* Shared memory region: 16 × 4KB rings = 64KB */
uintptr_t console_rings_vaddr;

static console_session_t sessions[MAX_CONSOLE_SESSIONS];
static uint32_t session_count = 0;

static uint32_t active_pd = 0;        /* currently attached PD id */
static bool     has_active = false;    /* is anything attached? */
static uint32_t display_mode = MODE_BROADCAST;  /* start in broadcast */

/* PD name table — maps pd_id to friendly name */
static const struct { uint32_t id; const char *name; } pd_names[] = {
    { 0,  "controller" },
    { 1,  "event_bus"  },
    { 2,  "init_agent" },
    { 3,  "agentfs"    },
    { 4,  "vibe_engine"},
    { 5,  "worker_0"   },
    { 6,  "worker_1"   },
    { 7,  "worker_2"   },
    { 8,  "worker_3"   },
    { 9,  "swap_slot_0"},
    { 10, "swap_slot_1"},
    { 11, "swap_slot_2"},
    { 12, "swap_slot_3"},
    { 13, "console_mux"},
    { 14, "linux_vmm"  },
    { 15, "fault_hndlr"},
};
#define NUM_PD_NAMES (sizeof(pd_names) / sizeof(pd_names[0]))

/* ─── Helpers ─────────────────────────────────────────────────────────── */

static volatile console_ring_header_t *get_ring(uint32_t slot) {
    return (volatile console_ring_header_t *)
        (console_rings_vaddr + (slot * RING_SIZE));
}

static volatile char *get_ring_buf(uint32_t slot) {
    return (volatile char *)
        (console_rings_vaddr + (slot * RING_SIZE) + RING_HEADER_SIZE);
}

static const char *pd_name_for(uint32_t pd_id) {
    for (uint32_t i = 0; i < NUM_PD_NAMES; i++) {
        if (pd_names[i].id == pd_id) return pd_names[i].name;
    }
    return "unknown";
}

static console_session_t *find_session(uint32_t pd_id) {
    for (uint32_t i = 0; i < session_count; i++) {
        if (sessions[i].active && sessions[i].pd_id == pd_id)
            return &sessions[i];
    }
    return NULL;
}

static console_session_t *get_or_create_session(uint32_t pd_id) {
    console_session_t *s = find_session(pd_id);
    if (s) return s;
    
    if (session_count >= MAX_CONSOLE_SESSIONS) return NULL;
    
    s = &sessions[session_count++];
    s->active = true;
    s->pd_id = pd_id;
    s->bytes_total = 0;
    s->lines_total = 0;
    s->scrollback.line_head = 0;
    s->scrollback.line_count = 0;
    
    /* Copy name */
    const char *name = pd_name_for(pd_id);
    uint32_t i;
    for (i = 0; i < 15 && name[i]; i++) {
        s->name[i] = name[i];
    }
    s->name[i] = '\0';
    
    return s;
}

/* ─── UART Output ─────────────────────────────────────────────────────── */

static void uart_putc(char c) {
    char buf[2] = { c, '\0' };
    microkit_dbg_puts(buf);
}

static void uart_puts(const char *s) {
    microkit_dbg_puts(s);
}

/* Print a tagged line: "[pd_name] text\n" */
static void uart_tagged_line(const char *pd_name, const char *line) {
    uart_puts("\033[36m[");  /* cyan tag */
    uart_puts(pd_name);
    uart_puts("]\033[0m ");
    uart_puts(line);
    uart_puts("\n");
}

/* Print separator bar */
static void uart_separator(void) {
    uart_puts("\033[90m────────────────────────────────────\033[0m\n");
}

/* Print attach banner */
static void uart_attach_banner(const char *pd_name) {
    uart_puts("\n\033[1;33m╔══════════════════════════════════╗\n");
    uart_puts("║  agentOS console: \033[1;37m");
    uart_puts(pd_name);
    /* Pad to fixed width */
    uint32_t len = 0;
    for (const char *p = pd_name; *p; p++) len++;
    for (uint32_t i = len; i < 14; i++) uart_putc(' ');
    uart_puts("\033[1;33m ║\n");
    uart_puts("╚══════════════════════════════════╝\033[0m\n\n");
}

/* ─── Scrollback ──────────────────────────────────────────────────────── */

static void scrollback_add_line(scrollback_t *sb, const char *line, uint32_t len) {
    uint32_t slot = sb->line_head % MAX_SCROLLBACK_LINES;
    uint32_t copy_len = (len < MAX_LINE_LEN - 1) ? len : (MAX_LINE_LEN - 1);
    
    for (uint32_t i = 0; i < copy_len; i++) {
        sb->lines[slot][i] = line[i];
    }
    sb->lines[slot][copy_len] = '\0';
    
    sb->line_head++;
    if (sb->line_count < MAX_SCROLLBACK_LINES) {
        sb->line_count++;
    }
}

static void scrollback_replay(scrollback_t *sb, const char *pd_name, uint32_t max_lines) {
    if (sb->line_count == 0) return;
    
    uint32_t start;
    uint32_t count = (max_lines < sb->line_count) ? max_lines : sb->line_count;
    
    if (sb->line_count < MAX_SCROLLBACK_LINES) {
        start = (sb->line_head > count) ? sb->line_head - count : 0;
    } else {
        start = sb->line_head - count;
    }
    
    uart_puts("\033[90m--- scrollback (");
    /* Print count as decimal */
    char nbuf[8];
    int ni = 7;
    nbuf[ni] = '\0';
    uint32_t cv = count;
    if (cv == 0) { nbuf[--ni] = '0'; }
    else { while (cv > 0 && ni > 0) { nbuf[--ni] = '0' + (cv % 10); cv /= 10; } }
    uart_puts(&nbuf[ni]);
    uart_puts(" lines) ---\033[0m\n");
    
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (start + i) % MAX_SCROLLBACK_LINES;
        uart_tagged_line(pd_name, sb->lines[slot]);
    }
    uart_separator();
}

/* ─── Ring Drain ──────────────────────────────────────────────────────── */

/*
 * Drain a PD's ring buffer. Reads characters from tail to head,
 * accumulates into line buffer, and dispatches complete lines.
 * Returns number of bytes drained.
 */
static uint32_t drain_ring(uint32_t slot, console_session_t *session, bool show) {
    volatile console_ring_header_t *hdr = get_ring(slot);
    volatile char *buf = get_ring_buf(slot);
    
    if (hdr->magic != CONSOLE_RING_MAGIC) return 0;
    
    uint32_t head = hdr->head;
    uint32_t tail = hdr->tail;
    uint32_t drained = 0;
    
    /* Line accumulator */
    static char line_buf[MAX_LINE_LEN];
    static uint32_t line_pos = 0;
    
    while (tail != head) {
        char c = buf[tail % RING_BUF_SIZE];
        tail = (tail + 1) % RING_BUF_SIZE;
        drained++;
        
        if (c == '\n' || line_pos >= MAX_LINE_LEN - 1) {
            line_buf[line_pos] = '\0';
            
            /* Add to scrollback */
            scrollback_add_line(&session->scrollback, line_buf, line_pos);
            session->lines_total++;
            
            /* Output if this session should be shown */
            if (show) {
                if (display_mode == MODE_BROADCAST || display_mode == MODE_SPLIT) {
                    uart_tagged_line(session->name, line_buf);
                } else {
                    /* Single mode: raw output */
                    uart_puts(line_buf);
                    uart_puts("\n");
                }
            }
            
            line_pos = 0;
        } else {
            line_buf[line_pos++] = c;
        }
    }
    
    /* Update tail pointer */
    hdr->tail = tail;
    session->bytes_total += drained;
    
    return drained;
}

/* Drain all active rings */
static void drain_all(void) {
    for (uint32_t i = 0; i < session_count; i++) {
        if (!sessions[i].active) continue;
        
        bool show;
        switch (display_mode) {
            case MODE_SINGLE:
                show = has_active && (sessions[i].pd_id == active_pd);
                break;
            case MODE_BROADCAST:
                show = true;
                break;
            case MODE_SPLIT:
                show = true;  /* split shows all, side-by-side tags */
                break;
            default:
                show = false;
        }
        
        drain_ring(i, &sessions[i], show);
    }
}

/* ─── Ring Initialization ─────────────────────────────────────────────── */

static void init_rings(void) {
    for (uint32_t i = 0; i < MAX_CONSOLE_SESSIONS; i++) {
        volatile console_ring_header_t *hdr = get_ring(i);
        hdr->magic = 0;
        hdr->pd_id = 0;
        hdr->head = 0;
        hdr->tail = 0;
    }
}

/* Register a PD's ring (called when a PD first writes) */
static void register_ring(uint32_t slot, uint32_t pd_id) {
    volatile console_ring_header_t *hdr = get_ring(slot);
    hdr->magic = CONSOLE_RING_MAGIC;
    hdr->pd_id = pd_id;
    hdr->head = 0;
    hdr->tail = 0;
    
    get_or_create_session(pd_id);
    
    if (display_mode == MODE_BROADCAST) {
        uart_puts("\033[32m[+] ");
        uart_puts(pd_name_for(pd_id));
        uart_puts(" connected to console\033[0m\n");
    }
}

/* ─── IPC Handlers ────────────────────────────────────────────────────── */

static void handle_attach(uint32_t pd_id) {
    console_session_t *s = find_session(pd_id);
    if (!s) {
        microkit_dbg_puts("[console_mux] attach failed: no session for PD\n");
        microkit_mr_set(0, 1);  /* error */
        return;
    }
    
    active_pd = pd_id;
    has_active = true;
    display_mode = MODE_SINGLE;
    
    uart_attach_banner(s->name);
    
    /* Replay scrollback */
    scrollback_replay(&s->scrollback, s->name, 20);
    
    microkit_mr_set(0, 0);  /* success */
}

static void handle_detach(void) {
    if (has_active) {
        uart_puts("\033[33m[~] detached from ");
        uart_puts(pd_name_for(active_pd));
        uart_puts("\033[0m\n");
    }
    
    has_active = false;
    display_mode = MODE_BROADCAST;
    
    uart_puts("\033[33m[~] switched to broadcast mode\033[0m\n");
    microkit_mr_set(0, 0);
}

static void handle_list(void) {
    uint32_t bitmask = 0;
    
    uart_puts("\n\033[1;37m agentOS Console Sessions\033[0m\n");
    uart_separator();
    
    for (uint32_t i = 0; i < session_count; i++) {
        if (!sessions[i].active) continue;
        bitmask |= (1u << sessions[i].pd_id);
        
        bool is_attached = has_active && (sessions[i].pd_id == active_pd);
        
        if (is_attached) uart_puts("\033[1;32m→ ");
        else uart_puts("  ");
        
        /* Print pd_id */
        char id_buf[4];
        int id_i = 3;
        id_buf[id_i] = '\0';
        uint32_t v = sessions[i].pd_id;
        if (v == 0) { id_buf[--id_i] = '0'; }
        else { while (v > 0 && id_i > 0) { id_buf[--id_i] = '0' + (v % 10); v /= 10; } }
        uart_puts(&id_buf[id_i]);
        
        uart_puts(": ");
        uart_puts(sessions[i].name);
        
        /* Pad name */
        uint32_t nlen = 0;
        for (const char *p = sessions[i].name; *p; p++) nlen++;
        for (uint32_t j = nlen; j < 14; j++) uart_putc(' ');
        
        uart_puts(" (");
        /* Print lines_total */
        char ln_buf[12];
        int ln_i = 11;
        ln_buf[ln_i] = '\0';
        uint64_t lv = sessions[i].lines_total;
        if (lv == 0) { ln_buf[--ln_i] = '0'; }
        else { while (lv > 0 && ln_i > 0) { ln_buf[--ln_i] = '0' + (lv % 10); lv /= 10; } }
        uart_puts(&ln_buf[ln_i]);
        uart_puts(" lines)");
        
        if (is_attached) uart_puts(" ← attached\033[0m");
        uart_puts("\n");
    }
    
    uart_separator();
    
    microkit_mr_set(0, 0);
    microkit_mr_set(1, bitmask);
}

static void handle_mode(uint32_t mode) {
    if (mode > MODE_SPLIT) {
        microkit_mr_set(0, 1);
        return;
    }
    
    display_mode = mode;
    
    const char *mode_names[] = { "single", "broadcast", "split" };
    uart_puts("\033[33m[~] display mode: ");
    uart_puts(mode_names[mode]);
    uart_puts("\033[0m\n");
    
    microkit_mr_set(0, 0);
}

static void handle_status(void) {
    microkit_mr_set(0, 0);
    microkit_mr_set(1, has_active ? active_pd : 0xFFFFFFFF);
    microkit_mr_set(2, display_mode);
    microkit_mr_set(3, session_count);
}

static void handle_scroll(uint32_t lines) {
    if (!has_active) {
        microkit_mr_set(0, 1);
        return;
    }
    
    console_session_t *s = find_session(active_pd);
    if (!s) {
        microkit_mr_set(0, 1);
        return;
    }
    
    scrollback_replay(&s->scrollback, s->name, lines);
    microkit_mr_set(0, 0);
}

/* ─── Microkit Entry Points ───────────────────────────────────────────── */

void init(void) {
    /* Use uart_puts directly — console_mux must NOT call console_log() on itself,
     * because console_log() PPCs on CH_CONSOLEMUX which is a channel FROM other PDs
     * INTO console_mux, not a channel that console_mux holds for outbound calls.
     * Calling console_log() here would trigger "invalid channel" and corrupt the
     * caller's IPC state. */
    uart_puts("[console_mux] starting — agentOS session multiplexer\n");

    init_rings();

    /* Start in broadcast mode — show all PDs */
    display_mode = MODE_BROADCAST;
    has_active = false;
    session_count = 0;

    uart_puts("\n");
    uart_puts("\033[1;35m╔══════════════════════════════════════╗\n");
    uart_puts("║     agentOS Console Multiplexer      ║\n");
    uart_puts("║  ");
    uart_puts(AGENTOS_VERSION_STR);
    uart_puts("      ║\n");
    uart_puts("║  mode: broadcast | sessions: 0       ║\n");
    uart_puts("╚══════════════════════════════════════╝\033[0m\n\n");

    uart_puts("[console_mux] ready — broadcast mode, waiting for PD connections\n");
}

void notified(microkit_channel ch) {
    /*
     * Notification channels:
     *   ch 0: controller commands (also triggers drain)
     *   ch 1..15: PD output ready (drain specific ring)
     *
     * On any notification, we drain all active rings.
     * This is simple and correct — a smarter version could
     * drain only the notifying PD's ring.
     */
    (void)ch;
    drain_all();
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    /*
     * Protected procedure call from controller (ch 0).
     * Op code in MR0, args in MR1..MR4.
     */
    (void)ch;
    (void)msginfo;
    
    uint32_t op = (uint32_t)microkit_mr_get(0);
    
    switch (op) {
        case OP_CONSOLE_ATTACH:
            handle_attach((uint32_t)microkit_mr_get(1));
            break;
            
        case OP_CONSOLE_DETACH:
            handle_detach();
            break;
            
        case OP_CONSOLE_LIST:
            handle_list();
            break;
            
        case OP_CONSOLE_MODE:
            handle_mode((uint32_t)microkit_mr_get(1));
            break;
            
        case OP_CONSOLE_SCROLL:
            handle_scroll((uint32_t)microkit_mr_get(1));
            break;
            
        case OP_CONSOLE_STATUS:
            handle_status();
            break;
            
        case OP_CONSOLE_WRITE: {
            /* A PD is registering its ring and requesting a drain */
            uint32_t slot = (uint32_t)microkit_mr_get(1);
            uint32_t pd_id = (uint32_t)microkit_mr_get(2);
            if (slot < MAX_CONSOLE_SESSIONS) {
                volatile console_ring_header_t *hdr = get_ring(slot);
                if (hdr->magic != CONSOLE_RING_MAGIC) {
                    register_ring(slot, pd_id);
                }
                console_session_t *s = find_session(pd_id);
                if (s) {
                    bool show = (display_mode == MODE_BROADCAST) ||
                                (has_active && pd_id == active_pd);
                    drain_ring(slot, s, show);
                }
            }
            microkit_mr_set(0, 0);
            break;
        }
            
        default:
            microkit_dbg_puts("[console_mux] unknown op\n");
            microkit_mr_set(0, 0xFF);
            break;
    }
    
    return microkit_msginfo_new(0, 4);
}
