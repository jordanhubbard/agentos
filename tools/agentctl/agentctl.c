/*
 * agentctl — agentOS Interactive Launcher & CC Client
 *
 * Pre-boot mode (default):
 *   Detect available QEMU binaries, then present an ncurses menu to select
 *   architecture, board, guest OS, and launch options.
 *   Constructs and exec()s the appropriate qemu-system-* command.
 *
 * Post-boot mode (-s / --sessions):
 *   Connects to the CC PD bridge socket (cc_bridge.sock) and exercises the
 *   full cc_contract.h MSG_CC_* API surface.  All post-boot operations route
 *   exclusively through cc_pd — no direct calls to service PDs.
 *
 *   Implements: session management, guest/device listing, framebuffer attach,
 *   keyboard/mouse input injection, snapshot/restore, and live log streaming.
 *
 * Build:
 *   cc -o agentctl agentctl.c $(pkg-config --cflags --libs ncurses)
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <ncurses.h>

/* ─── Constants ──────────────────────────────────────────────────────────── */

#define AGENTCTL_VERSION  "0.2.0"
#define MAX_CMD_LEN       4096
#define CC_BRIDGE_SOCK    "build/cc_bridge.sock"

/* Color pair IDs */
#define CP_NORMAL    1   /* default text */
#define CP_HEADER    2   /* title bar */
#define CP_SELECTED  3   /* selected menu item (reverse) */
#define CP_AVAIL     4   /* available / affirmative */
#define CP_DISABLED  5   /* grayed-out / unavailable */
#define CP_DESC      6   /* description pane text */
#define CP_HINT      7   /* bottom key-hint bar */
#define CP_BORDER    8   /* box-drawing borders */

/* ─── Architecture table ─────────────────────────────────────────────────── */

typedef struct {
    const char *id;
    const char *label;
    const char *desc;
    const char *qemu;
} arch_def_t;

static const arch_def_t ARCHES[] = {
    {
        "riscv64",
        "RISC-V 64-bit  [riscv64]",
        "Open ISA reference platform.\n"
        "Boots via OpenSBI firmware + QEMU virt machine.\n"
        "No virtualization extensions needed — fastest to test.\n"
        "\n"
        "Supports: native agentOS PDs only (no VMM guests).",
        "qemu-system-riscv64"
    },
    {
        "aarch64",
        "ARM64 (AArch64) [aarch64]",
        "64-bit ARM with EL2 hypervisor mode.\n"
        "Enables full Linux VMM and FreeBSD VMM guest support\n"
        "via au-ts/libvmm and seL4's hardware virtualization.\n"
        "\n"
        "Supports: agentOS + Linux VMM + FreeBSD VMM.",
        "qemu-system-aarch64"
    },
    {
        "x86_64",
        "x86-64           [x86_64]",
        "Intel/AMD 64-bit platform.\n"
        "KVM acceleration available on x86 Linux hosts.\n"
        "Linux VMM is a stub (libvmm x86 support in progress).\n"
        "\n"
        "Supports: native agentOS PDs + Linux VMM stub.",
        "qemu-system-x86_64"
    },
};
#define N_ARCHES 3

/* ─── Board table ────────────────────────────────────────────────────────── */

typedef struct {
    const char *id;
    const char *label;
    const char *arch;
    const char *desc;
} board_def_t;

static const board_def_t BOARDS[] = {
    {
        "qemu_virt_riscv64",
        "qemu_virt_riscv64  (RISC-V QEMU virt)",
        "riscv64",
        "Standard QEMU RISC-V virt machine.\n"
        "Boots via: -bios opensbi-riscv64-generic-fw_dynamic.bin -kernel <img>\n"
        "Memory: up to 8GB. No virtualization extensions required."
    },
    {
        "qemu_virt_aarch64",
        "qemu_virt_aarch64  (AArch64 QEMU virt)",
        "aarch64",
        "ARM virt machine with EL2 hypervisor extensions.\n"
        "Flags: virtualization=on, highmem=off, secure=off.\n"
        "CPU: cortex-a53. Required for VMM guest support.\n"
        "Image loaded via: -device loader,addr=0x70000000."
    },
    {
        "x86_64_generic",
        "x86_64_generic     (x86-64 QEMU q35)",
        "x86_64",
        "Q35 chipset with qemu64 CPU.\n"
        "KVM acceleration enabled automatically if /dev/kvm is available.\n"
        "Microkit image loaded with -kernel flag."
    },
};
#define N_BOARDS 3

/* ─── Guest OS table ─────────────────────────────────────────────────────── */

typedef struct {
    const char *id;
    const char *label;
    const char *desc;
    const char *elf_name;   /* NULL = always available */
} guest_def_t;

static const guest_def_t GUESTS[] = {
    {
        "none",
        "agentOS only         (no guest VM)",
        "Runs native agentOS protection domains only:\n"
        "  controller, event_bus, init_agent, workers,\n"
        "  vibe_engine, agentfs, console_mux, net_isolator.\n"
        "\n"
        "Available on all architectures.",
        NULL
    },
    {
        "linux",
        "+Linux VMM           (libvmm guest)",
        "Boots a Linux kernel inside a seL4 virtual machine\n"
        "via au-ts/libvmm. Provides a Linux userland for agents\n"
        "that need full POSIX compatibility.\n"
        "\n"
        "Requires: aarch64 build. linux_vmm.elf in build/.",
        "linux_vmm.elf"
    },
    {
        "freebsd",
        "+FreeBSD VMM         (libvmm UEFI guest)",
        "Boots FreeBSD 14 AArch64 via EDK2 UEFI firmware\n"
        "and au-ts/libvmm. Supports up to 4 FreeBSD VM slots\n"
        "managed by the VM multiplexer PD.\n"
        "\n"
        "Requires: aarch64 build. freebsd_vmm.elf in build/.",
        "freebsd_vmm.elf"
    },
};
#define N_GUESTS 3

/* ─── Memory options ─────────────────────────────────────────────────────── */

static const char *MEM_OPTS[] = { "256M", "512M", "1G", "2G", "4G" };
#define N_MEM_OPTS  5
#define DEFAULT_MEM 3   /* 2G */

/* ─── Launch configuration ───────────────────────────────────────────────── */

typedef struct {
    int  arch;
    int  board;
    int  guest;
    int  mem;
    bool kvm;
    bool gdb;
    bool dry_run;
} cfg_t;

/* ─── Screen state ───────────────────────────────────────────────────────── */

typedef enum {
    SCREEN_ARCH = 0,
    SCREEN_BOARD,
    SCREEN_GUEST,
    SCREEN_OPTIONS,
    SCREEN_CONFIRM,
    SCREEN_DONE,
} screen_t;

/* ─── System detection ───────────────────────────────────────────────────── */

static bool qemu_available[N_ARCHES];

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void detect_qemu(void)
{
    for (int i = 0; i < N_ARCHES; i++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "command -v %s >/dev/null 2>&1", ARCHES[i].qemu);
        qemu_available[i] = (system(cmd) == 0);
    }
}

static bool kvm_available(void)
{
    return file_exists("/dev/kvm");
}

static bool guest_elf_exists(int guest_idx, int board_idx)
{
    if (GUESTS[guest_idx].elf_name == NULL)
        return true;
    char path[512];
    snprintf(path, sizeof(path), "build/%s/%s",
             BOARDS[board_idx].id, GUESTS[guest_idx].elf_name);
    return file_exists(path);
}

/* ─── ncurses color init ─────────────────────────────────────────────────── */

static void init_colors(void)
{
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(CP_NORMAL,   COLOR_WHITE,   -1);
    init_pair(CP_HEADER,   COLOR_BLACK,   COLOR_CYAN);
    init_pair(CP_SELECTED, COLOR_BLACK,   COLOR_WHITE);
    init_pair(CP_AVAIL,    COLOR_GREEN,   -1);
    init_pair(CP_DISABLED, COLOR_BLACK+8, -1);
    init_pair(CP_DESC,     COLOR_CYAN,    -1);
    init_pair(CP_HINT,     COLOR_BLACK,   COLOR_WHITE);
    init_pair(CP_BORDER,   COLOR_CYAN,    -1);
}

/* ─── Common UI elements ─────────────────────────────────────────────────── */

static void draw_header(const char *right_label)
{
    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvhline(0, 0, ' ', COLS);
    mvprintw(0, 2, "agentctl v%s  —  agentOS Launcher", AGENTCTL_VERSION);
    if (right_label && right_label[0]) {
        int rx = COLS - (int)strlen(right_label) - 3;
        if (rx > 0)
            mvprintw(0, rx, "%s", right_label);
    }
    attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
}

static void draw_hints(const char *hints)
{
    attron(COLOR_PAIR(CP_HINT));
    mvhline(LINES - 1, 0, ' ', COLS);
    mvprintw(LINES - 1, 1, "%s", hints);
    attroff(COLOR_PAIR(CP_HINT));
}

static void draw_box(int y, int x, int h, int w, const char *title)
{
    attron(COLOR_PAIR(CP_BORDER));
    mvaddch(y,         x,         ACS_ULCORNER);
    mvaddch(y,         x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x,         ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    mvhline(y,         x + 1, ACS_HLINE, w - 2);
    mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
    for (int i = 1; i < h - 1; i++) {
        mvaddch(y + i, x,         ACS_VLINE);
        mvaddch(y + i, x + w - 1, ACS_VLINE);
    }
    if (title && title[0]) {
        char buf[64];
        snprintf(buf, sizeof(buf), " %s ", title);
        int tx = x + (w - (int)strlen(buf)) / 2;
        if (tx < x + 1) tx = x + 1;
        attron(A_BOLD);
        mvprintw(y, tx, "%s", buf);
        attroff(A_BOLD);
    }
    attroff(COLOR_PAIR(CP_BORDER));
}

/*
 * Print text inside a box with simple word-wrap.
 * Stops at max_row. Honours '\n' as a line break.
 */
static void draw_wrapped(const char *text, int y, int x, int w, int max_row)
{
    if (!text) return;
    attron(COLOR_PAIR(CP_DESC));
    const char *p = text;
    while (*p && y < max_row) {
        const char *nl = strchr(p, '\n');
        int seg = nl ? (int)(nl - p) : (int)strlen(p);
        /* split segment into lines of width w */
        while (seg > 0 && y < max_row) {
            int chunk = seg < w ? seg : w;
            mvprintw(y, x, "%.*s", chunk, p);
            p += chunk;
            seg -= chunk;
            y++;
        }
        if (nl) { p = nl + 1; }
        else     { break; }
    }
    attroff(COLOR_PAIR(CP_DESC));
}

/* ─── Generic list menu ─────────────────────────────────────────────────────
 *
 * labels[]  — item text strings
 * n         — number of items
 * avail[]   — item enabled flags (NULL = all enabled)
 * sel_io    — in: initial selection; out: final selection
 * desc_fn   — description callback for right pane (may be NULL)
 *
 * Returns: selected index on Enter, -1 on back (q/Backspace), -2 on quit (Q)
 */
static int menu_screen(const char    *title,
                       const char   **labels,
                       int            n,
                       const bool    *avail,
                       int           *sel_io,
                       const char   *(*desc_fn)(int))
{
    int sel = *sel_io;
    if (sel < 0 || sel >= n) sel = 0;
    /* Advance to first available item */
    for (int i = 0; i < n; i++) {
        if (!avail || avail[(sel + i) % n]) { sel = (sel + i) % n; break; }
    }

    int list_w  = COLS * 6 / 10;
    int desc_x  = list_w + 2;
    int desc_w  = COLS - desc_x - 1;
    int box_y   = 2;
    int box_h   = LINES - 4;
    int inner_h = box_h - 2;

    while (1) {
        clear();
        draw_header(title);
        draw_box(box_y, 1, box_h, list_w, "Select");
        draw_hints("  \u2191\u2193 Navigate   Enter Confirm   Backspace/q Back   Q Quit");

        /* Scroll to keep selection visible */
        int scroll = 0;
        if (sel >= inner_h) scroll = sel - inner_h + 1;

        for (int i = 0; i < n; i++) {
            if (i < scroll || (i - scroll) >= inner_h) continue;
            int row       = box_y + 1 + (i - scroll);
            bool is_sel   = (i == sel);
            bool is_avail = (!avail || avail[i]);

            /* Clear row */
            mvhline(row, 2, ' ', list_w - 3);

            if (is_sel && is_avail)
                attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else if (!is_avail)
                attron(COLOR_PAIR(CP_DISABLED));
            else
                attron(COLOR_PAIR(CP_NORMAL));

            mvprintw(row, 2, " %c %s", is_sel ? '>' : ' ', labels[i]);
            if (!is_avail) {
                int px = list_w - 19;
                if (px > 4) mvprintw(row, px, "[not available]");
            }

            if (is_sel && is_avail)
                attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else if (!is_avail)
                attroff(COLOR_PAIR(CP_DISABLED));
            else
                attroff(COLOR_PAIR(CP_NORMAL));
        }

        /* Description pane */
        if (desc_w > 12 && desc_fn) {
            draw_box(box_y, desc_x, box_h, desc_w, "Info");
            const char *d = desc_fn(sel);
            if (d)
                draw_wrapped(d, box_y + 2, desc_x + 2, desc_w - 4,
                             box_y + box_h - 2);
        }

        refresh();

        int ch = getch();
        switch (ch) {
        case KEY_UP:
            for (int i = 1; i <= n; i++) {
                int t = (sel - i + n) % n;
                if (!avail || avail[t]) { sel = t; break; }
            }
            break;
        case KEY_DOWN:
            for (int i = 1; i <= n; i++) {
                int t = (sel + i) % n;
                if (!avail || avail[t]) { sel = t; break; }
            }
            break;
        case '\n': case KEY_ENTER:
            if (!avail || avail[sel]) {
                *sel_io = sel;
                return sel;
            }
            break;
        case KEY_BACKSPACE: case 127: case '\b': case 'q':
            *sel_io = sel;
            return -1;
        case 'Q':
            return -2;
        }
    }
}

/* ─── Description callbacks ─────────────────────────────────────────────── */

static const char *arch_desc(int i)
{
    return (i >= 0 && i < N_ARCHES) ? ARCHES[i].desc  : NULL;
}
static const char *board_desc(int i)
{
    return (i >= 0 && i < N_BOARDS) ? BOARDS[i].desc  : NULL;
}
static const char *guest_desc(int i)
{
    return (i >= 0 && i < N_GUESTS) ? GUESTS[i].desc  : NULL;
}

/* ─── Options screen ─────────────────────────────────────────────────────── */

/*
 * Returns 1 to proceed, -1 to go back, -2 to quit.
 */
static int options_screen(cfg_t *cfg)
{
    bool has_kvm = kvm_available();
    int  sel     = 0;

#define N_OPT_ROWS 5   /* mem, kvm, gdb, dry_run, Continue */

    while (1) {
        clear();
        draw_header("Options");
        draw_hints(
            "  \u2191\u2193 Navigate   Space/Enter Toggle or Confirm   "
            "\u2190\u2192 Cycle memory   Backspace/q Back");

        int y = 3;
        mvprintw(y, 2, "Configure launch options:");
        y += 2;

        /* Helper lambda — draws one option row */
#define OPT_ROW(idx, fmt, ...) do {                         \
        bool _s = (sel == (idx));                           \
        if (_s) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);  \
        else    attron(COLOR_PAIR(CP_NORMAL));              \
        mvhline(y, 2, ' ', COLS - 4);                      \
        mvprintw(y, 2, " %c  " fmt, _s ? '>' : ' ',        \
                 ##__VA_ARGS__);                             \
        if (_s) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD); \
        else    attroff(COLOR_PAIR(CP_NORMAL));             \
        y += 2;                                             \
} while (0)

        /* Memory row */
        {
            bool _s = (sel == 0);
            if (_s) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else    attron(COLOR_PAIR(CP_NORMAL));
            mvhline(y, 2, ' ', COLS - 4);
            mvprintw(y, 2, " %c  Memory : ", _s ? '>' : ' ');
            for (int m = 0; m < N_MEM_OPTS; m++) {
                if (m == cfg->mem) attron(A_UNDERLINE);
                printw("[%s]", MEM_OPTS[m]);
                if (m == cfg->mem) attroff(A_UNDERLINE);
                if (m < N_MEM_OPTS - 1) printw(" ");
            }
            if (_s) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else    attroff(COLOR_PAIR(CP_NORMAL));
            y += 2;
        }

        /* KVM row */
        {
            bool _s = (sel == 1);
            bool on = has_kvm && cfg->kvm;
            if (_s && has_kvm)
                attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else if (!has_kvm)
                attron(COLOR_PAIR(CP_DISABLED));
            else
                attron(COLOR_PAIR(CP_NORMAL));
            mvhline(y, 2, ' ', COLS - 4);
            mvprintw(y, 2, " %c  KVM accel  : [%s]%s",
                     _s ? '>' : ' ',
                     on ? "ON " : "OFF",
                     has_kvm ? "" : "  (no /dev/kvm)");
            if (_s && has_kvm)
                attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else if (!has_kvm)
                attroff(COLOR_PAIR(CP_DISABLED));
            else
                attroff(COLOR_PAIR(CP_NORMAL));
            y += 2;
        }

        OPT_ROW(2, "GDB stub   : [%s]  (-s -S, tcp::1234, stopped at boot)",
                cfg->gdb ? "ON " : "OFF");
        OPT_ROW(3, "Dry run    : [%s]  (print command, do not exec)",
                cfg->dry_run ? "ON " : "OFF");

        /* Continue button */
        {
            bool _s = (sel == 4);
            if (_s) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else    attron(COLOR_PAIR(CP_AVAIL)  | A_BOLD);
            mvhline(y, 2, ' ', COLS - 4);
            mvprintw(y, 2, " %c  Continue  \u2192", _s ? '>' : ' ');
            if (_s) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else    attroff(COLOR_PAIR(CP_AVAIL)  | A_BOLD);
        }

        refresh();

        int ch = getch();
        switch (ch) {
        case KEY_UP:    if (sel > 0)            sel--; break;
        case KEY_DOWN:  if (sel < N_OPT_ROWS-1) sel++; break;
        case KEY_LEFT:
            if (sel == 0 && cfg->mem > 0)              cfg->mem--; break;
        case KEY_RIGHT:
            if (sel == 0 && cfg->mem < N_MEM_OPTS - 1) cfg->mem++; break;
        case ' ': case '\n': case KEY_ENTER:
            switch (sel) {
            case 0: cfg->mem = (cfg->mem + 1) % N_MEM_OPTS; break;
            case 1: if (has_kvm) cfg->kvm = !cfg->kvm;      break;
            case 2: cfg->gdb     = !cfg->gdb;                break;
            case 3: cfg->dry_run = !cfg->dry_run;            break;
            case 4: return 1;
            }
            break;
        case KEY_BACKSPACE: case 127: case '\b': case 'q': return -1;
        case 'Q': return -2;
        }
    }
#undef N_OPT_ROWS
#undef OPT_ROW
}

/* ─── QEMU command builder ───────────────────────────────────────────────── */

static void build_qemu_cmd(const cfg_t *cfg, char *cmd, size_t len)
{
    const char *arch   = ARCHES[cfg->arch].id;
    const char *board  = BOARDS[cfg->board].id;
    const char *qemu   = ARCHES[cfg->arch].qemu;
    const char *mem    = MEM_OPTS[cfg->mem];
    const char *gdb    = cfg->gdb ? " -s -S" : "";

    char image[512];
    snprintf(image, sizeof(image), "build/%s/agentos.img", board);

    if (strcmp(arch, "aarch64") == 0) {
        snprintf(cmd, len,
                 "%s"
                 " -machine virt,virtualization=on,highmem=off,secure=off"
                 " -cpu cortex-a53"
                 " -m %s"
                 " -serial mon:stdio"
                 " -nographic"
                 " -device loader,file=%s,addr=0x70000000,cpu-num=0"
                 "%s",
                 qemu, mem, image, gdb);
    } else if (strcmp(arch, "x86_64") == 0) {
        snprintf(cmd, len,
                 "%s"
                 " -machine q35"
                 " -cpu qemu64"
                 " -m %s"
                 " -nographic"
                 " -serial mon:stdio"
                 "%s"
                 " -kernel %s"
                 "%s",
                 qemu, mem,
                 cfg->kvm ? " -enable-kvm" : "",
                 image, gdb);
    } else {
        /* riscv64 */
        snprintf(cmd, len,
                 "%s"
                 " -machine virt"
                 " -cpu rv64"
                 " -m %s"
                 " -nographic"
                 " -bios /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin"
                 " -kernel %s"
                 "%s",
                 qemu, mem, image, gdb);
    }
}

/* ─── Confirm screen ─────────────────────────────────────────────────────── */

/*
 * Returns 1 to launch, -1 to go back, -2 to quit.
 */
static int confirm_screen(const cfg_t *cfg)
{
    char cmd[MAX_CMD_LEN];
    build_qemu_cmd(cfg, cmd, sizeof(cmd));

    while (1) {
        clear();
        draw_header("Confirm & Launch");
        draw_hints(cfg->dry_run
            ? "  Enter Exit   Backspace/q Back   Q Quit"
            : "  Enter LAUNCH agentOS   Backspace/q Back   Q Quit");

        int y = 2;
        attron(A_BOLD);
        mvprintw(y++, 2, "Configuration:");
        attroff(A_BOLD);
        y++;
        mvprintw(y++, 4, "Arch     : %s", ARCHES[cfg->arch].label);
        mvprintw(y++, 4, "Board    : %s", BOARDS[cfg->board].id);
        mvprintw(y++, 4, "Guest OS : %s (%s)",
                 GUESTS[cfg->guest].id, GUESTS[cfg->guest].label);
        mvprintw(y++, 4, "Memory   : %s", MEM_OPTS[cfg->mem]);
        if (cfg->kvm)     mvprintw(y++, 4, "KVM      : enabled");
        if (cfg->gdb)     mvprintw(y++, 4, "GDB stub : enabled (tcp::1234, boot halted)");
        if (cfg->dry_run) {
            attron(COLOR_PAIR(CP_DISABLED) | A_BOLD);
            mvprintw(y++, 4, "Dry run  : ON — command will not be executed");
            attroff(COLOR_PAIR(CP_DISABLED) | A_BOLD);
        }
        y++;

        attron(A_BOLD);
        mvprintw(y++, 2, "QEMU command:");
        attroff(A_BOLD);
        y++;

        attron(COLOR_PAIR(CP_AVAIL) | A_BOLD);
        /* Wrap long command at terminal width */
        int maxw = COLS - 6;
        const char *p = cmd;
        while (*p && y < LINES - 3) {
            int chunk = (int)strlen(p);
            if (chunk > maxw) chunk = maxw;
            /* Try to break at a space */
            if (chunk == maxw && p[chunk] != '\0' && p[chunk] != ' ') {
                for (int i = chunk - 1; i > maxw / 2; i--) {
                    if (p[i] == ' ') { chunk = i; break; }
                }
            }
            mvprintw(y++, 4, "%.*s", chunk, p);
            p += chunk;
            while (*p == ' ') p++;
        }
        attroff(COLOR_PAIR(CP_AVAIL) | A_BOLD);
        y++;

        if (cfg->dry_run) {
            attron(COLOR_PAIR(CP_DISABLED) | A_BOLD);
            mvprintw(y, 2, "[ DRY RUN ] Press Enter to exit. "
                           "Copy the command above to run manually.");
            attroff(COLOR_PAIR(CP_DISABLED) | A_BOLD);
        } else {
            attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            mvprintw(y, 2,
                     "[ Press Enter to launch ]   (Ctrl-A X to quit QEMU later)");
            attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
        }

        refresh();

        int ch = getch();
        switch (ch) {
        case '\n': case KEY_ENTER:
            return 1;
        case KEY_BACKSPACE: case 127: case '\b': case 'q':
            return -1;
        case 'Q':
            return -2;
        }
    }
}

/* ─── Pre-boot menu flow ─────────────────────────────────────────────────── */

static int run_preboot_menu(void)
{
    cfg_t cfg = {
        .arch    = 0,
        .board   = 0,
        .guest   = 0,
        .mem     = DEFAULT_MEM,
        .kvm     = kvm_available(),
        .gdb     = false,
        .dry_run = false,
    };

    screen_t screen = SCREEN_ARCH;

    while (screen != SCREEN_DONE) {
        int ret;

        switch (screen) {

        /* ── Arch selection ── */
        case SCREEN_ARCH: {
            const char *labels[N_ARCHES];
            bool        avail[N_ARCHES];
            for (int i = 0; i < N_ARCHES; i++) {
                labels[i] = ARCHES[i].label;
                avail[i]  = qemu_available[i];
            }
            ret = menu_screen("Architecture", labels, N_ARCHES,
                              avail, &cfg.arch, arch_desc);
            if (ret == -2) return 0;
            if (ret >= 0) {
                /* Auto-map arch → board */
                for (int i = 0; i < N_BOARDS; i++) {
                    if (strcmp(BOARDS[i].arch, ARCHES[cfg.arch].id) == 0) {
                        cfg.board = i;
                        break;
                    }
                }
                /* Reset guest selection */
                cfg.guest = 0;
                screen = SCREEN_BOARD;
            }
            /* ret == -1 on first screen: ignore */
            break;
        }

        /* ── Board selection ── */
        case SCREEN_BOARD: {
            const char *labels[N_BOARDS];
            bool        avail[N_BOARDS];
            for (int i = 0; i < N_BOARDS; i++) {
                labels[i] = BOARDS[i].label;
                avail[i]  = (strcmp(BOARDS[i].arch,
                                    ARCHES[cfg.arch].id) == 0);
            }
            ret = menu_screen("Board", labels, N_BOARDS,
                              avail, &cfg.board, board_desc);
            if (ret == -2) return 0;
            if (ret >= 0) screen = SCREEN_GUEST;
            if (ret == -1) screen = SCREEN_ARCH;
            break;
        }

        /* ── Guest OS selection ── */
        case SCREEN_GUEST: {
            const char *labels[N_GUESTS];
            bool        avail[N_GUESTS];
            for (int i = 0; i < N_GUESTS; i++) {
                labels[i] = GUESTS[i].label;
                if (i == 0) {
                    /* "agentOS only" always available */
                    avail[i] = true;
                } else if (strcmp(ARCHES[cfg.arch].id, "aarch64") != 0) {
                    /* VMM guests require aarch64 (x86 stub shown separately) */
                    avail[i] = false;
                } else {
                    avail[i] = guest_elf_exists(i, cfg.board);
                }
            }
            ret = menu_screen("Guest OS", labels, N_GUESTS,
                              avail, &cfg.guest, guest_desc);
            if (ret == -2) return 0;
            if (ret >= 0) screen = SCREEN_OPTIONS;
            if (ret == -1) screen = SCREEN_BOARD;
            break;
        }

        /* ── Options ── */
        case SCREEN_OPTIONS:
            ret = options_screen(&cfg);
            if (ret == -2) return 0;
            if (ret >  0)  screen = SCREEN_CONFIRM;
            if (ret == -1) screen = SCREEN_GUEST;
            break;

        /* ── Confirm ── */
        case SCREEN_CONFIRM:
            ret = confirm_screen(&cfg);
            if (ret == -2) return 0;
            if (ret >  0)  screen = SCREEN_DONE;
            if (ret == -1) screen = SCREEN_OPTIONS;
            break;

        default:
            screen = SCREEN_DONE;
            break;
        }
    }

    /* Build final QEMU command */
    char cmd[MAX_CMD_LEN];
    build_qemu_cmd(&cfg, cmd, sizeof(cmd));

    /* Tear down ncurses before exec/print */
    endwin();

    if (cfg.dry_run) {
        printf("\n[agentctl] Dry run — QEMU command:\n\n  %s\n\n", cmd);
        return 0;
    }

    printf("\n[agentctl] Launching agentOS...\n");
    printf("  %s\n\n", cmd);
    printf("  (Ctrl-A X to quit QEMU)\n\n");
    fflush(stdout);

    /* Split command string into argv for execvp */
    char     cmd_copy[MAX_CMD_LEN];
    strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';

    const char *argv[128];
    int         argc = 0;
    char       *tok  = strtok(cmd_copy, " ");
    while (tok && argc < 127) {
        argv[argc++] = tok;
        tok = strtok(NULL, " ");
    }
    argv[argc] = NULL;

    execvp(argv[0], (char *const *)argv);
    perror("execvp");
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CC Contract — host-side types and constants (mirrors cc_contract.h)
 *
 * cc_contract.h depends on agentos.h (seL4/Microkit), which is not available
 * on the host.  We replicate the types and opcodes here so agentctl can be
 * built as a standalone POSIX tool.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* MSG_CC_* opcodes (from agentos.h) */
#define MSG_CC_CONNECT              0x2601u
#define MSG_CC_DISCONNECT           0x2602u
#define MSG_CC_SEND                 0x2603u
#define MSG_CC_RECV                 0x2604u
#define MSG_CC_STATUS               0x2605u
#define MSG_CC_LIST                 0x2606u
#define MSG_CC_LIST_GUESTS          0x2607u
#define MSG_CC_LIST_DEVICES         0x2608u
#define MSG_CC_LIST_POLECATS        0x2609u
#define MSG_CC_GUEST_STATUS         0x260Au
#define MSG_CC_DEVICE_STATUS        0x260Bu
#define MSG_CC_ATTACH_FRAMEBUFFER   0x260Cu
#define MSG_CC_SEND_INPUT           0x260Du
#define MSG_CC_SNAPSHOT             0x260Eu
#define MSG_CC_RESTORE              0x260Fu
#define MSG_CC_LOG_STREAM           0x2610u

/* CC_* error codes */
#define CC_OK                   0u
#define CC_ERR_NO_SESSIONS      1u
#define CC_ERR_BAD_SESSION      2u
#define CC_ERR_EXPIRED          3u
#define CC_ERR_CMD_TOO_LARGE    4u
#define CC_ERR_NO_RESPONSE      5u
#define CC_ERR_BAD_HANDLE       6u
#define CC_ERR_BAD_DEV_TYPE     7u
#define CC_ERR_RELAY_FAULT      8u

/* CC_SESSION_STATE_* */
#define CC_SESSION_STATE_CONNECTED  0u
#define CC_SESSION_STATE_IDLE       1u
#define CC_SESSION_STATE_BUSY       2u
#define CC_SESSION_STATE_EXPIRED    3u

/* CC_DEV_TYPE_* */
#define CC_DEV_TYPE_SERIAL  0u
#define CC_DEV_TYPE_NET     1u
#define CC_DEV_TYPE_BLOCK   2u
#define CC_DEV_TYPE_USB     3u
#define CC_DEV_TYPE_FB      4u
#define CC_DEV_TYPE_COUNT   5u

/* CC_INPUT_* */
#define CC_INPUT_KEY_DOWN   0x01u
#define CC_INPUT_KEY_UP     0x02u
#define CC_INPUT_MOUSE_MOVE 0x03u
#define CC_INPUT_MOUSE_BTN  0x04u

/* VIBEOS_TYPE_* and VIBEOS_ARCH_* (from vibeos_contract.h) */
#define VIBEOS_TYPE_LINUX   0x01u
#define VIBEOS_TYPE_FREEBSD 0x02u
#define VIBEOS_ARCH_AARCH64 0x01u
#define VIBEOS_ARCH_X86_64  0x02u

/* GUEST_STATE_* (from guest_contract.h) */
#define GUEST_STATE_CREATING   0u
#define GUEST_STATE_BINDING    1u
#define GUEST_STATE_READY      2u
#define GUEST_STATE_BOOTING    3u
#define GUEST_STATE_RUNNING    4u
#define GUEST_STATE_SUSPENDED  5u
#define GUEST_STATE_DEAD       6u

/* EVENT_FB_FRAME_READY (EventBus event kind) */
#define EVENT_FB_FRAME_READY    0x40u

/* CC_MAX_* */
#define CC_MAX_SESSIONS     8u
#define CC_MAX_RESP_BYTES   4096u

/* ─── Shmem entry types (from cc_contract.h) ─────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t session_id;
    uint32_t state;
    uint32_t client_badge;
    uint32_t ticks_since_active;
} cc_session_info_t;

typedef struct __attribute__((packed)) {
    uint32_t guest_handle;
    uint32_t state;
    uint32_t os_type;
    uint32_t arch;
} cc_guest_info_t;

typedef struct __attribute__((packed)) {
    uint32_t guest_handle;
    uint32_t state;
    uint32_t os_type;
    uint32_t arch;
    uint32_t device_flags;
    uint32_t _reserved[3];
} cc_guest_status_t;

typedef struct __attribute__((packed)) {
    uint32_t dev_type;
    uint32_t dev_handle;
    uint32_t state;
    uint32_t _reserved;
} cc_device_info_t;

typedef struct __attribute__((packed)) {
    uint32_t event_type;   /* CC_INPUT_* */
    uint32_t keycode;      /* HID usage code */
    int32_t  dx;
    int32_t  dy;
    uint32_t btn_mask;
    uint32_t _reserved;
} cc_input_event_t;

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t frame_seq;
    uint32_t backend;
    uint32_t _reserved;
} fb_frame_ready_event_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Wire protocol
 *
 * Binary framing over the cc_bridge Unix socket.  Each request / response
 * maps directly to the seL4 Microkit MR layout used by cc_pd.
 *
 * Request:  [magic:u32][opcode:u32][mr1:u32][mr2:u32][mr3:u32][shmem_len:u32]
 *           [shmem_data: shmem_len bytes]
 *
 * Response: [magic:u32][mr0:u32][mr1:u32][mr2:u32][mr3:u32][shmem_len:u32]
 *           [shmem_data: shmem_len bytes]
 *
 * Event (server-pushed): [magic_event:u32][event_type:u32][data: 32 bytes]
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CC_WIRE_MAGIC       0xCC4A0000u
#define CC_WIRE_EVENT_MAGIC 0xCC4A0001u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t opcode;
    uint32_t mr[3];      /* MR1, MR2, MR3 */
    uint32_t shmem_len;
} cc_wire_req_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t mr[4];      /* MR0=status, MR1, MR2, MR3 */
    uint32_t shmem_len;
} cc_wire_resp_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;        /* CC_WIRE_EVENT_MAGIC */
    uint32_t event_type;
    uint8_t  data[32];
} cc_wire_event_t;

/* ─── CC client state ───────────────────────────────────────────────────── */

typedef struct {
    int      fd;
    bool     connected;
    uint32_t session_id;
    uint8_t  shmem[CC_MAX_RESP_BYTES];
} cc_client_t;

/* ─── Low-level I/O helpers ─────────────────────────────────────────────── */

static int readn(int fd, void *buf, size_t n)
{
    size_t   done = 0;
    uint8_t *p    = (uint8_t *)buf;
    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int writen(int fd, const void *buf, size_t n)
{
    size_t         done = 0;
    const uint8_t *p    = (const uint8_t *)buf;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return 0;
}

static int cc_open_sock(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ─── Wire transaction ──────────────────────────────────────────────────── */

/*
 * Send one request, receive one response.  shmem_in is optional (pass NULL /
 * 0 for calls with no outbound shmem data).  Response shmem is stored in
 * cc->shmem.  Returns 0 on success, -1 on I/O or protocol error.
 */
static int cc_transact(cc_client_t *cc,
                       uint32_t     opcode,
                       uint32_t     mr1, uint32_t mr2, uint32_t mr3,
                       const void  *shmem_in, uint32_t shmem_in_len,
                       cc_wire_resp_t *resp_out)
{
    cc_wire_req_t req;
    memset(&req, 0, sizeof(req));
    req.magic      = CC_WIRE_MAGIC;
    req.opcode     = opcode;
    req.mr[0]      = mr1;
    req.mr[1]      = mr2;
    req.mr[2]      = mr3;
    req.shmem_len  = shmem_in_len;

    if (writen(cc->fd, &req, sizeof(req)) < 0)
        return -1;
    if (shmem_in_len > 0 && shmem_in)
        if (writen(cc->fd, shmem_in, shmem_in_len) < 0)
            return -1;

    cc_wire_resp_t resp;
    if (readn(cc->fd, &resp, sizeof(resp)) < 0)
        return -1;
    if (resp.magic != CC_WIRE_MAGIC)
        return -1;

    if (resp.shmem_len > CC_MAX_RESP_BYTES) {
        /* Drain oversized payload to keep framing aligned */
        uint8_t discard[256];
        uint32_t rem = resp.shmem_len;
        while (rem > 0) {
            uint32_t chunk = rem < (uint32_t)sizeof(discard)
                             ? rem : (uint32_t)sizeof(discard);
            if (readn(cc->fd, discard, chunk) < 0) return -1;
            rem -= chunk;
        }
        resp.shmem_len = 0;
    } else if (resp.shmem_len > 0) {
        if (readn(cc->fd, cc->shmem, resp.shmem_len) < 0)
            return -1;
    }

    *resp_out = resp;
    return 0;
}

/*
 * Non-blocking check for a server-pushed event frame.
 * Returns 1 if an event was read into *ev, 0 if nothing pending, -1 on error.
 */
static int cc_poll_event(cc_client_t *cc, cc_wire_event_t *ev)
{
    struct pollfd pfd = { .fd = cc->fd, .events = POLLIN };
    int r = poll(&pfd, 1, 0);
    if (r <= 0) return r;

    uint32_t magic;
    if (readn(cc->fd, &magic, sizeof(magic)) < 0) return -1;
    if (magic != CC_WIRE_EVENT_MAGIC) return -1;

    ev->magic = magic;
    if (readn(cc->fd, &ev->event_type, sizeof(ev->event_type)) < 0) return -1;
    if (readn(cc->fd, ev->data, sizeof(ev->data)) < 0)              return -1;
    return 1;
}

/* ─── CC opcode wrappers ─────────────────────────────────────────────────── */

static int cc_do_connect(cc_client_t *cc)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_CONNECT, (uint32_t)getpid(), 0, 0,
                    NULL, 0, &r) < 0)
        return -1;
    if (r.mr[0] != CC_OK) return -(int)r.mr[0];
    cc->session_id = r.mr[1];
    cc->connected  = true;
    return 0;
}

static int cc_do_disconnect(cc_client_t *cc)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_DISCONNECT, cc->session_id, 0, 0,
                    NULL, 0, &r) < 0)
        return -1;
    cc->connected = false;
    return (r.mr[0] == CC_OK) ? 0 : -(int)r.mr[0];
}

static int cc_do_status(cc_client_t *cc,
                        uint32_t *state, uint32_t *pending, uint32_t *ticks)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_STATUS, cc->session_id, 0, 0,
                    NULL, 0, &r) < 0)
        return -1;
    if (r.mr[0] != CC_OK) return -(int)r.mr[0];
    if (state)   *state   = r.mr[1];
    if (pending) *pending = r.mr[2];
    if (ticks)   *ticks   = r.mr[3];
    return 0;
}

static int cc_do_list_sessions(cc_client_t *cc,
                               cc_session_info_t *out, uint32_t max,
                               uint32_t *count)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_LIST, 0, 0, 0, NULL, 0, &r) < 0)
        return -1;
    uint32_t n = r.mr[0];
    if (count) *count = n;
    if (n > max) n = max;
    uint32_t copy = n * (uint32_t)sizeof(cc_session_info_t);
    if (copy > r.shmem_len) copy = r.shmem_len;
    memcpy(out, cc->shmem, copy);
    return 0;
}

static int cc_do_list_guests(cc_client_t *cc,
                             cc_guest_info_t *out, uint32_t max,
                             uint32_t *count)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_LIST_GUESTS, 0, 0, 0, NULL, 0, &r) < 0)
        return -1;
    uint32_t n = r.mr[0];
    if (count) *count = n;
    if (n > max) n = max;
    uint32_t copy = n * (uint32_t)sizeof(cc_guest_info_t);
    if (copy > r.shmem_len) copy = r.shmem_len;
    memcpy(out, cc->shmem, copy);
    return 0;
}

static int cc_do_list_devices(cc_client_t *cc, uint32_t dev_type,
                              cc_device_info_t *out, uint32_t max,
                              uint32_t *count)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_LIST_DEVICES, dev_type, 0, 0,
                    NULL, 0, &r) < 0)
        return -1;
    uint32_t n = r.mr[0];
    if (count) *count = n;
    if (n > max) n = max;
    uint32_t copy = n * (uint32_t)sizeof(cc_device_info_t);
    if (copy > r.shmem_len) copy = r.shmem_len;
    memcpy(out, cc->shmem, copy);
    return 0;
}

static int cc_do_list_polecats(cc_client_t *cc,
                               uint32_t *total, uint32_t *busy, uint32_t *idle)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_LIST_POLECATS, 0, 0, 0, NULL, 0, &r) < 0)
        return -1;
    if (r.mr[0] != CC_OK) return -(int)r.mr[0];
    if (total) *total = r.mr[1];
    if (busy)  *busy  = r.mr[2];
    if (idle)  *idle  = r.mr[3];
    return 0;
}

static int cc_do_guest_status(cc_client_t *cc, uint32_t guest_handle,
                              cc_guest_status_t *out)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_GUEST_STATUS, guest_handle, 0, 0,
                    NULL, 0, &r) < 0)
        return -1;
    if (r.mr[0] != CC_OK) return -(int)r.mr[0];
    if (out && r.shmem_len >= sizeof(*out))
        memcpy(out, cc->shmem, sizeof(*out));
    return 0;
}

static int cc_do_device_status(cc_client_t *cc,
                               uint32_t dev_type, uint32_t dev_handle,
                               uint32_t raw[3])
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_DEVICE_STATUS, dev_type, dev_handle, 0,
                    NULL, 0, &r) < 0)
        return -1;
    if (r.mr[0] != CC_OK) return -(int)r.mr[0];
    if (raw && r.shmem_len >= 3 * sizeof(uint32_t))
        memcpy(raw, cc->shmem, 3 * sizeof(uint32_t));
    return 0;
}

static int cc_do_attach_framebuffer(cc_client_t *cc,
                                    uint32_t guest_handle, uint32_t fb_handle,
                                    uint32_t *frame_seq)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_ATTACH_FRAMEBUFFER,
                    guest_handle, fb_handle, 0, NULL, 0, &r) < 0)
        return -1;
    if (r.mr[0] != CC_OK) return -(int)r.mr[0];
    if (frame_seq) *frame_seq = r.mr[1];
    return 0;
}

static int cc_do_send_input(cc_client_t *cc, uint32_t guest_handle,
                            const cc_input_event_t *ev)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_SEND_INPUT, guest_handle, 0, 0,
                    ev, sizeof(*ev), &r) < 0)
        return -1;
    return (r.mr[0] == CC_OK) ? 0 : -(int)r.mr[0];
}

static int cc_do_snapshot(cc_client_t *cc, uint32_t guest_handle,
                          uint32_t *snap_lo, uint32_t *snap_hi)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_SNAPSHOT, guest_handle, 0, 0,
                    NULL, 0, &r) < 0)
        return -1;
    if (r.mr[0] != CC_OK) return -(int)r.mr[0];
    if (snap_lo) *snap_lo = r.mr[1];
    if (snap_hi) *snap_hi = r.mr[2];
    return 0;
}

static int cc_do_restore(cc_client_t *cc, uint32_t guest_handle,
                         uint32_t snap_lo, uint32_t snap_hi)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_RESTORE, guest_handle, snap_lo, snap_hi,
                    NULL, 0, &r) < 0)
        return -1;
    return (r.mr[0] == CC_OK) ? 0 : -(int)r.mr[0];
}

static int cc_do_log_stream(cc_client_t *cc, uint32_t slot, uint32_t pd_id,
                            uint32_t *bytes_drained)
{
    cc_wire_resp_t r;
    if (cc_transact(cc, MSG_CC_LOG_STREAM, slot, pd_id, 0,
                    NULL, 0, &r) < 0)
        return -1;
    if (r.mr[0] != CC_OK) return -(int)r.mr[0];
    if (bytes_drained) *bytes_drained = r.mr[1];
    return 0;
}

/* ─── Label helpers ─────────────────────────────────────────────────────── */

static const char *guest_state_name(uint32_t s)
{
    switch (s) {
    case GUEST_STATE_CREATING:  return "CREATING";
    case GUEST_STATE_BINDING:   return "BINDING";
    case GUEST_STATE_READY:     return "READY";
    case GUEST_STATE_BOOTING:   return "BOOTING";
    case GUEST_STATE_RUNNING:   return "RUNNING";
    case GUEST_STATE_SUSPENDED: return "SUSPENDED";
    case GUEST_STATE_DEAD:      return "DEAD";
    default:                    return "UNKNOWN";
    }
}

static const char *guest_os_name(uint32_t t)
{
    switch (t) {
    case VIBEOS_TYPE_LINUX:   return "Linux";
    case VIBEOS_TYPE_FREEBSD: return "FreeBSD";
    default:                  return "Unknown";
    }
}

static const char *guest_arch_name(uint32_t a)
{
    switch (a) {
    case VIBEOS_ARCH_AARCH64: return "AArch64";
    case VIBEOS_ARCH_X86_64:  return "x86-64";
    default:                  return "Unknown";
    }
}

static const char *dev_type_name(uint32_t t)
{
    switch (t) {
    case CC_DEV_TYPE_SERIAL: return "Serial";
    case CC_DEV_TYPE_NET:    return "Net";
    case CC_DEV_TYPE_BLOCK:  return "Block";
    case CC_DEV_TYPE_USB:    return "USB";
    case CC_DEV_TYPE_FB:     return "Framebuffer";
    default:                 return "Unknown";
    }
}

static const char *session_state_name(uint32_t s)
{
    switch (s) {
    case CC_SESSION_STATE_CONNECTED: return "CONNECTED";
    case CC_SESSION_STATE_IDLE:      return "IDLE";
    case CC_SESSION_STATE_BUSY:      return "BUSY";
    case CC_SESSION_STATE_EXPIRED:   return "EXPIRED";
    default:                         return "UNKNOWN";
    }
}

static const char *cc_err_name(uint32_t e)
{
    switch (e) {
    case CC_OK:               return "OK";
    case CC_ERR_NO_SESSIONS:  return "NO_SESSIONS";
    case CC_ERR_BAD_SESSION:  return "BAD_SESSION";
    case CC_ERR_EXPIRED:      return "EXPIRED";
    case CC_ERR_CMD_TOO_LARGE:return "CMD_TOO_LARGE";
    case CC_ERR_NO_RESPONSE:  return "NO_RESPONSE";
    case CC_ERR_BAD_HANDLE:   return "BAD_HANDLE";
    case CC_ERR_BAD_DEV_TYPE: return "BAD_DEV_TYPE";
    case CC_ERR_RELAY_FAULT:  return "RELAY_FAULT";
    default:                  return "UNKNOWN";
    }
}

/* ─── UI helpers for CC screens ─────────────────────────────────────────── */

/* Draw a one-line status bar at row y showing session info */
static void draw_cc_statusbar(cc_client_t *cc, int y)
{
    attron(COLOR_PAIR(CP_DISABLED));
    mvhline(y, 0, ' ', COLS);
    mvprintw(y, 2, "CC  session:%u  fd:%d  %s",
             cc->session_id, cc->fd,
             cc->connected ? "connected" : "disconnected");
    attroff(COLOR_PAIR(CP_DISABLED));
}

/* Show a transient error message and wait for keypress */
static void show_cc_error(const char *title, int err)
{
    clear();
    draw_header(title);
    draw_hints("  Any key to return");
    int y = LINES / 2 - 2;
    attron(COLOR_PAIR(CP_DISABLED) | A_BOLD);
    mvprintw(y,   COLS/2 - 12, "CC error: %s (%d)",
             err < 0 ? cc_err_name((uint32_t)-err) : "I/O error", err);
    mvprintw(y+2, COLS/2 - 14, "cc_pd returned an error — see cc_contract.h");
    attroff(COLOR_PAIR(CP_DISABLED) | A_BOLD);
    refresh();
    getch();
}

/* ─── Screen: Sessions (MSG_CC_LIST + MSG_CC_STATUS) ─────────────────────── */

static void screen_sessions(cc_client_t *cc)
{
    cc_session_info_t sess[CC_MAX_SESSIONS];
    uint32_t count = 0;
    int err = cc_do_list_sessions(cc, sess, CC_MAX_SESSIONS, &count);

    uint32_t my_state = 0, pending = 0, ticks = 0;
    cc_do_status(cc, &my_state, &pending, &ticks);

    while (1) {
        clear();
        draw_header("Sessions");
        draw_hints("  r Refresh   q Back");
        draw_cc_statusbar(cc, LINES - 2);

        int y = 2;
        if (err < 0) {
            attron(COLOR_PAIR(CP_DISABLED));
            mvprintw(y, 4, "Error fetching sessions: %s",
                     cc_err_name((uint32_t)-err));
            attroff(COLOR_PAIR(CP_DISABLED));
        } else {
            attron(A_BOLD);
            mvprintw(y++, 2, "Active CC sessions  (%u total, %u max)",
                     count, CC_MAX_SESSIONS);
            attroff(A_BOLD);
            y++;

            mvprintw(y++, 2, "My session: id=%u  state=%s  pending=%u  "
                     "ticks_idle=%u",
                     cc->session_id, session_state_name(my_state),
                     pending, ticks);
            y++;

            draw_box(y, 2, count + 2 < 6 ? 6 : count + 2,
                     COLS - 4, "All Sessions");
            y++;

            attron(COLOR_PAIR(CP_DESC));
            mvprintw(y++, 4, "%-6s  %-12s  %-8s  %s",
                     "ID", "STATE", "BADGE", "TICKS_IDLE");
            attroff(COLOR_PAIR(CP_DESC));

            for (uint32_t i = 0; i < count; i++) {
                bool mine = (sess[i].session_id == cc->session_id);
                if (mine) attron(COLOR_PAIR(CP_AVAIL) | A_BOLD);
                mvprintw(y++, 4, "%-6u  %-12s  %-8u  %u%s",
                         sess[i].session_id,
                         session_state_name(sess[i].state),
                         sess[i].client_badge,
                         sess[i].ticks_since_active,
                         mine ? "  ← this session" : "");
                if (mine) attroff(COLOR_PAIR(CP_AVAIL) | A_BOLD);
            }
            if (count == 0) {
                attron(COLOR_PAIR(CP_DISABLED));
                mvprintw(y, 4, "(no sessions)");
                attroff(COLOR_PAIR(CP_DISABLED));
            }
        }

        refresh();
        int ch = getch();
        if (ch == 'r' || ch == 'R') {
            err   = cc_do_list_sessions(cc, sess, CC_MAX_SESSIONS, &count);
            cc_do_status(cc, &my_state, &pending, &ticks);
        } else {
            break;
        }
    }
}

/* ─── Screen: Polecats (MSG_CC_LIST_POLECATS) ────────────────────────────── */

static void screen_polecats(cc_client_t *cc)
{
    uint32_t total = 0, busy = 0, idle = 0;
    int err = cc_do_list_polecats(cc, &total, &busy, &idle);

    while (1) {
        clear();
        draw_header("Polecats");
        draw_hints("  r Refresh   q Back");
        draw_cc_statusbar(cc, LINES - 2);

        int y = 3;
        attron(A_BOLD);
        mvprintw(y++, 2, "Agent Pool Status (MSG_CC_LIST_POLECATS)");
        attroff(A_BOLD);
        y++;

        if (err < 0) {
            attron(COLOR_PAIR(CP_DISABLED));
            mvprintw(y, 4, "Relay error: %s", cc_err_name((uint32_t)-err));
            attroff(COLOR_PAIR(CP_DISABLED));
        } else {
            draw_box(y, 2, 7, 40, "Agent Pool");
            y++;
            attron(COLOR_PAIR(CP_AVAIL) | A_BOLD);
            mvprintw(y++, 4, "Total  : %u", total);
            attroff(COLOR_PAIR(CP_AVAIL) | A_BOLD);
            mvprintw(y++, 4, "Busy   : %u", busy);
            attron(COLOR_PAIR(CP_AVAIL));
            mvprintw(y++, 4, "Idle   : %u", idle);
            attroff(COLOR_PAIR(CP_AVAIL));
            y++;
            attron(COLOR_PAIR(CP_DESC));
            mvprintw(y, 4, "Routed via cc_pd → agent_pool "
                     "(MSG_AGENTPOOL_STATUS)");
            attroff(COLOR_PAIR(CP_DESC));
        }

        refresh();
        int ch = getch();
        if (ch == 'r' || ch == 'R')
            err = cc_do_list_polecats(cc, &total, &busy, &idle);
        else
            break;
    }
}

/* ─── Screen: Devices (MSG_CC_LIST_DEVICES + MSG_CC_DEVICE_STATUS) ───────── */

static void screen_devices(cc_client_t *cc)
{
    static const uint32_t dev_types[CC_DEV_TYPE_COUNT] = {
        CC_DEV_TYPE_SERIAL, CC_DEV_TYPE_NET,
        CC_DEV_TYPE_BLOCK,  CC_DEV_TYPE_USB, CC_DEV_TYPE_FB,
    };

    int sel = 0;
    while (1) {
        clear();
        draw_header("Devices");
        draw_hints("  \u2191\u2193 Select type   Enter List   d Device status"
                   "   q Back");
        draw_cc_statusbar(cc, LINES - 2);

        int y = 3;
        attron(A_BOLD);
        mvprintw(y++, 2, "Device types  (MSG_CC_LIST_DEVICES / "
                 "MSG_CC_DEVICE_STATUS)");
        attroff(A_BOLD);
        y++;

        for (int i = 0; i < (int)CC_DEV_TYPE_COUNT; i++) {
            bool s = (i == sel);
            if (s) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else   attron(COLOR_PAIR(CP_NORMAL));
            mvhline(y, 2, ' ', COLS - 4);
            mvprintw(y, 2, " %c  [%u] %s",
                     s ? '>' : ' ', dev_types[i],
                     dev_type_name(dev_types[i]));
            if (s) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else   attroff(COLOR_PAIR(CP_NORMAL));
            y++;
        }

        refresh();
        int ch = getch();
        switch (ch) {
        case KEY_UP:   if (sel > 0) sel--;                         break;
        case KEY_DOWN: if (sel < (int)CC_DEV_TYPE_COUNT - 1) sel++; break;
        case 'q': case 'Q': case KEY_BACKSPACE: case 127: return;
        case '\n': case KEY_ENTER: case 'd': {
            uint32_t dt = dev_types[sel];
            cc_device_info_t devs[16];
            uint32_t count = 0;
            int err = cc_do_list_devices(cc, dt, devs, 16, &count);

            clear();
            draw_header(dev_type_name(dt));
            draw_hints("  Any key to return");
            draw_cc_statusbar(cc, LINES - 2);

            int dy = 3;
            if (err < 0) {
                attron(COLOR_PAIR(CP_DISABLED));
                mvprintw(dy, 4, "Error: %s", cc_err_name((uint32_t)-err));
                attroff(COLOR_PAIR(CP_DISABLED));
            } else {
                attron(A_BOLD);
                mvprintw(dy++, 2, "%s devices: %u found",
                         dev_type_name(dt), count);
                attroff(A_BOLD);
                dy++;
                attron(COLOR_PAIR(CP_DESC));
                mvprintw(dy++, 4, "%-6s  %-8s  %s",
                         "TYPE", "HANDLE", "STATE");
                attroff(COLOR_PAIR(CP_DESC));
                for (uint32_t i = 0; i < count && dy < LINES - 4; i++) {
                    mvprintw(dy++, 4, "%-6s  %-8u  0x%08x",
                             dev_type_name(devs[i].dev_type),
                             devs[i].dev_handle, devs[i].state);
                }
                if (count == 0) {
                    attron(COLOR_PAIR(CP_DISABLED));
                    mvprintw(dy, 4, "(no devices of this type)");
                    attroff(COLOR_PAIR(CP_DISABLED));
                }

                /* If Enter was pressed, also call DEVICE_STATUS for handle 0 */
                if (ch == '\n' || ch == KEY_ENTER) {
                    uint32_t raw[3] = {0, 0, 0};
                    uint32_t handle = (count > 0) ? devs[0].dev_handle : 0;
                    int serr = cc_do_device_status(cc, dt, handle, raw);
                    dy++;
                    attron(A_BOLD);
                    mvprintw(dy++, 2, "MSG_CC_DEVICE_STATUS (handle %u):",
                             handle);
                    attroff(A_BOLD);
                    if (serr < 0) {
                        attron(COLOR_PAIR(CP_DISABLED));
                        mvprintw(dy, 4, "Error: %s",
                                 cc_err_name((uint32_t)-serr));
                        attroff(COLOR_PAIR(CP_DISABLED));
                    } else {
                        attron(COLOR_PAIR(CP_DESC));
                        mvprintw(dy, 4, "raw[0]=0x%08x  raw[1]=0x%08x"
                                 "  raw[2]=0x%08x",
                                 raw[0], raw[1], raw[2]);
                        attroff(COLOR_PAIR(CP_DESC));
                    }
                }
            }
            refresh();
            getch();
            break;
        }
        }
    }
}

/* ─── Screen: Framebuffer (MSG_CC_ATTACH_FRAMEBUFFER + frame events) ─────── */

/*
 * Calls MSG_CC_ATTACH_FRAMEBUFFER to subscribe the session to frame-ready
 * events, then polls for CC_WIRE_EVENT_MAGIC frames from the bridge.
 * Displays a live frame counter and metadata.
 *
 * The pixel display layer is a placeholder: cc_bridge forwards only the
 * fb_frame_ready_event_t metadata; raw pixel data remains in the seL4
 * fb_shmem region and is not transferred over the wire protocol.
 */
static void screen_framebuffer(cc_client_t *cc, uint32_t guest_handle)
{
    uint32_t fb_handle = 0;
    uint32_t frame_seq = 0;
    int err = cc_do_attach_framebuffer(cc, guest_handle, fb_handle, &frame_seq);

    uint32_t frames_seen  = 0;
    uint32_t last_seq     = frame_seq;
    const char *spin      = "|/-\\";
    int         spin_i    = 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Non-blocking keyboard input for this screen */
    nodelay(stdscr, TRUE);

    while (1) {
        clear();
        draw_header("Framebuffer");
        draw_hints("  q Back  (frame events received from cc_pd via EventBus)");
        draw_cc_statusbar(cc, LINES - 2);

        int y = 3;
        attron(A_BOLD);
        mvprintw(y++, 2, "MSG_CC_ATTACH_FRAMEBUFFER  guest=0x%08x  fb=0x%08x",
                 guest_handle, fb_handle);
        attroff(A_BOLD);
        y++;

        if (err < 0) {
            attron(COLOR_PAIR(CP_DISABLED) | A_BOLD);
            mvprintw(y++, 4, "Attach failed: %s (%d)",
                     cc_err_name((uint32_t)-err), err);
            mvprintw(y++, 4, "Possible causes: guest not running, "
                     "invalid fb_handle, relay fault");
            attroff(COLOR_PAIR(CP_DISABLED) | A_BOLD);
        } else {
            draw_box(y, 2, 10, 56, "Frame Viewer");
            int bx = 4;
            y++;
            mvprintw(y++, bx, "Status      : attached — waiting for frames");
            mvprintw(y++, bx, "Initial seq : %u", frame_seq);
            mvprintw(y++, bx, "Current seq : %u", last_seq);
            attron(COLOR_PAIR(CP_AVAIL) | A_BOLD);
            mvprintw(y++, bx, "Frames seen : %u  %c",
                     frames_seen, spin[spin_i & 3]);
            attroff(COLOR_PAIR(CP_AVAIL) | A_BOLD);
            y++;
            attron(COLOR_PAIR(CP_DISABLED));
            mvprintw(y++, bx,
                     "[pixel data in seL4 fb_shmem — not transferred over wire]");
            attroff(COLOR_PAIR(CP_DISABLED));
            y += 2;
            attron(COLOR_PAIR(CP_DESC));
            mvprintw(y++, 2,
                     "Route: agentctl \u2192 cc_bridge \u2192 cc_pd "
                     "\u2192 framebuffer_pd (MSG_FB_FLIP probe)");
            mvprintw(y++, 2,
                     "Events: EventBus EVENT_FB_FRAME_READY (0x%02x) "
                     "\u2192 cc_bridge \u2192 CC_WIRE_EVENT_MAGIC push",
                     EVENT_FB_FRAME_READY);
            attroff(COLOR_PAIR(CP_DESC));
        }

        refresh();

        /* Poll for frame events from the bridge (100 ms timeout) */
        if (err >= 0) {
            cc_wire_event_t ev;
            struct pollfd pfd = { .fd = cc->fd, .events = POLLIN };
            int pr = poll(&pfd, 1, 100);
            if (pr > 0) {
                int er = cc_poll_event(cc, &ev);
                if (er == 1 && ev.event_type == EVENT_FB_FRAME_READY) {
                    const fb_frame_ready_event_t *frev =
                        (const fb_frame_ready_event_t *)ev.data;
                    last_seq = frev->frame_seq;
                    frames_seen++;
                    spin_i++;
                }
            }
        }

        int ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == KEY_BACKSPACE || ch == 27)
            break;
    }

    nodelay(stdscr, FALSE);
}

/* ─── Screen: Log Stream (MSG_CC_LOG_STREAM) ─────────────────────────────── */

#define LOG_RING_LINES   128
#define LOG_LINE_WIDTH   256

static void screen_log_stream(cc_client_t *cc)
{
    static char log_ring[LOG_RING_LINES][LOG_LINE_WIDTH];
    static int  log_head = 0;   /* next write position */
    static int  log_total = 0;  /* total lines ever written */
    int         scroll    = 0;

    /* Running drain state */
    uint32_t slot  = 0;
    uint32_t pd_id = 0;

    /* Append text to the log ring, splitting on newlines */
    const char *p     = (const char *)cc->shmem;

    nodelay(stdscr, TRUE);

    while (1) {
        /* Drain one slot per iteration */
        uint32_t drained = 0;
        int err = cc_do_log_stream(cc, slot, pd_id, &drained);
        if (err == 0 && drained > 0) {
            /* Parse shmem bytes as newline-terminated log lines */
            const char *q   = (const char *)cc->shmem;
            const char *end = q + drained;
            while (q < end) {
                const char *nl = (const char *)memchr(q, '\n',
                                 (size_t)(end - q));
                int seg = nl ? (int)(nl - q) : (int)(end - q);
                if (seg > LOG_LINE_WIDTH - 1) seg = LOG_LINE_WIDTH - 1;
                memcpy(log_ring[log_head], q, (size_t)seg);
                log_ring[log_head][seg] = '\0';
                log_head  = (log_head  + 1) % LOG_RING_LINES;
                log_total++;
                q = nl ? nl + 1 : end;
            }
            scroll = 0;  /* auto-scroll to bottom on new data */
        }
        /* Advance slot round-robin through 8 slots to drain all PDs */
        slot = (slot + 1) % 8;

        /* Render */
        clear();
        draw_header("Log Stream");
        draw_hints("  r Refresh   PgUp/PgDn Scroll   q Back");
        draw_cc_statusbar(cc, LINES - 2);

        int rows_avail = LINES - 5;
        int y_start    = 2;

        attron(A_BOLD);
        mvprintw(y_start, 2, "MSG_CC_LOG_STREAM — live drain  "
                 "(slot=%u pd_id=%u  total_lines=%d)",
                 slot, pd_id, log_total);
        attroff(A_BOLD);

        draw_box(y_start + 1, 1, rows_avail + 1, COLS - 2, "Log");

        int base = log_total > LOG_RING_LINES
                   ? log_head : 0;
        int n    = log_total > LOG_RING_LINES
                   ? LOG_RING_LINES : log_total;
        int view_start = n - rows_avail + scroll;
        if (view_start < 0) view_start = 0;
        if (view_start > n - 1) view_start = n > 0 ? n - 1 : 0;

        attron(COLOR_PAIR(CP_AVAIL));
        for (int i = 0; i < rows_avail && (view_start + i) < n; i++) {
            int idx = (base + view_start + i) % LOG_RING_LINES;
            mvprintw(y_start + 2 + i, 3, "%.*s",
                     COLS - 6, log_ring[idx]);
        }
        attroff(COLOR_PAIR(CP_AVAIL));

        if (n == 0) {
            attron(COLOR_PAIR(CP_DISABLED));
            mvprintw(y_start + 3, 4,
                     "(no log data yet — cc_pd relays to log_drain "
                     "via OP_LOG_WRITE)");
            attroff(COLOR_PAIR(CP_DISABLED));
        }

        refresh();

        /* Poll for input without blocking */
        struct pollfd pfd = { .fd = cc->fd, .events = POLLIN };
        poll(&pfd, 1, 200);

        int ch = getch();
        switch (ch) {
        case 'q': case 'Q': case KEY_BACKSPACE: case 27:
            nodelay(stdscr, FALSE);
            return;
        case 'r': case 'R':
            scroll = 0;
            break;
        case KEY_PPAGE: scroll -= rows_avail / 2; break;
        case KEY_NPAGE: scroll += rows_avail / 2; break;
        }
        (void)p;  /* shmem accessed inside cc_do_log_stream via cc->shmem */
    }
}

/* ─── Screen: Guest detail (status + actions) ────────────────────────────── */

static void screen_guest_detail(cc_client_t *cc, uint32_t guest_handle)
{
    cc_guest_status_t st;
    int err = cc_do_guest_status(cc, guest_handle, &st);

    char snap_label[64] = "(none)";
    uint32_t snap_lo = 0, snap_hi = 0;
    bool has_snap = false;

    int sel = 0;
#define N_GUEST_ACTIONS 5

    while (1) {
        clear();
        draw_header("Guest Detail");
        draw_hints("  \u2191\u2193 Navigate   Enter Select   q Back");
        draw_cc_statusbar(cc, LINES - 2);

        int y = 2;
        attron(A_BOLD);
        mvprintw(y++, 2, "Guest 0x%08x  (MSG_CC_GUEST_STATUS)", guest_handle);
        attroff(A_BOLD);
        y++;

        if (err < 0) {
            attron(COLOR_PAIR(CP_DISABLED));
            mvprintw(y, 4, "Error fetching status: %s",
                     cc_err_name((uint32_t)-err));
            attroff(COLOR_PAIR(CP_DISABLED));
        } else {
            mvprintw(y++, 4, "State     : %s", guest_state_name(st.state));
            mvprintw(y++, 4, "OS        : %s", guest_os_name(st.os_type));
            mvprintw(y++, 4, "Arch      : %s", guest_arch_name(st.arch));
            mvprintw(y++, 4, "Dev flags : 0x%08x", st.device_flags);
        }
        y += 2;

        attron(A_BOLD);
        mvprintw(y++, 2, "Actions:");
        attroff(A_BOLD);

        static const char *actions[N_GUEST_ACTIONS] = {
            "Refresh status  (MSG_CC_GUEST_STATUS)",
            "Snapshot        (MSG_CC_SNAPSHOT)",
            "Restore         (MSG_CC_RESTORE)",
            "Attach framebuffer (MSG_CC_ATTACH_FRAMEBUFFER)",
            "Inject input    (MSG_CC_SEND_INPUT)",
        };

        for (int i = 0; i < N_GUEST_ACTIONS; i++) {
            bool s = (i == sel);
            if (s) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else   attron(COLOR_PAIR(CP_NORMAL));
            mvhline(y, 2, ' ', COLS - 4);
            mvprintw(y, 2, " %c  %s", s ? '>' : ' ', actions[i]);
            if (i == 2 && has_snap) {
                mvprintw(y, COLS - 28, "  snap: %08x%08x",
                         snap_hi, snap_lo);
            }
            if (s) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else   attroff(COLOR_PAIR(CP_NORMAL));
            y++;
        }

        refresh();

        int ch = getch();
        switch (ch) {
        case KEY_UP:   if (sel > 0)                  sel--; break;
        case KEY_DOWN: if (sel < N_GUEST_ACTIONS - 1) sel++; break;
        case 'q': case 'Q': case KEY_BACKSPACE: case 127: return;
        case '\n': case KEY_ENTER:
            switch (sel) {
            case 0:
                err = cc_do_guest_status(cc, guest_handle, &st);
                break;
            case 1: {
                int serr = cc_do_snapshot(cc, guest_handle,
                                          &snap_lo, &snap_hi);
                if (serr == 0) {
                    snprintf(snap_label, sizeof(snap_label),
                             "%08x%08x", snap_hi, snap_lo);
                    has_snap = true;
                } else {
                    show_cc_error("Snapshot", serr);
                }
                break;
            }
            case 2:
                if (has_snap) {
                    int rerr = cc_do_restore(cc, guest_handle,
                                             snap_lo, snap_hi);
                    if (rerr != 0) show_cc_error("Restore", rerr);
                } else {
                    /* Show info: need a snapshot first */
                    clear();
                    draw_header("Restore");
                    draw_hints("  Any key to return");
                    attron(COLOR_PAIR(CP_DISABLED));
                    mvprintw(LINES/2, COLS/2 - 16,
                             "No snapshot available — take snapshot first");
                    attroff(COLOR_PAIR(CP_DISABLED));
                    refresh();
                    getch();
                }
                break;
            case 3:
                screen_framebuffer(cc, guest_handle);
                break;
            case 4: {
                /* Input injection: capture keypresses, send as CC_INPUT events */
                nodelay(stdscr, TRUE);
                clear();
                draw_header("Input Injection");
                draw_hints("  Type to inject keys   Esc to stop");
                attron(COLOR_PAIR(CP_DESC));
                mvprintw(3, 2, "MSG_CC_SEND_INPUT  guest=0x%08x", guest_handle);
                mvprintw(4, 2, "Keypresses are forwarded as "
                         "CC_INPUT_KEY_DOWN/UP events.");
                mvprintw(5, 2, "Keycodes are raw ncurses values (not HID "
                         "usage codes — bridge translates).");
                attroff(COLOR_PAIR(CP_DESC));
                int irow = 7;
                mvprintw(irow, 2, "Injected keys:");
                int icol = 18;
                refresh();
                while (1) {
                    int k = getch();
                    if (k == 27) break;
                    if (k == ERR) {
                        struct pollfd pfd2 = { .fd = cc->fd,
                                               .events = POLLIN };
                        poll(&pfd2, 1, 50);
                        continue;
                    }
                    cc_input_event_t ev_down, ev_up;
                    memset(&ev_down, 0, sizeof(ev_down));
                    memset(&ev_up,   0, sizeof(ev_up));
                    ev_down.event_type = CC_INPUT_KEY_DOWN;
                    ev_down.keycode    = (uint32_t)k;
                    ev_up.event_type   = CC_INPUT_KEY_UP;
                    ev_up.keycode      = (uint32_t)k;
                    cc_do_send_input(cc, guest_handle, &ev_down);
                    cc_do_send_input(cc, guest_handle, &ev_up);
                    if (k >= 32 && k < 127) {
                        if (icol < COLS - 4) {
                            mvaddch(irow, icol++, (chtype)k);
                            refresh();
                        }
                    }
                }
                nodelay(stdscr, FALSE);
                break;
            }
            }
            break;
        }
    }
#undef N_GUEST_ACTIONS
}

/* ─── Screen: Guests (MSG_CC_LIST_GUESTS) ────────────────────────────────── */

static void screen_guests(cc_client_t *cc)
{
    cc_guest_info_t guests[16];
    uint32_t count = 0;
    int err = cc_do_list_guests(cc, guests, 16, &count);
    int sel = 0;

    while (1) {
        clear();
        draw_header("Guests");
        draw_hints("  \u2191\u2193 Navigate   Enter Detail   r Refresh   q Back");
        draw_cc_statusbar(cc, LINES - 2);

        int y = 3;
        attron(A_BOLD);
        mvprintw(y++, 2, "Guest VMs (MSG_CC_LIST_GUESTS)  —  %u found",
                 count);
        attroff(A_BOLD);
        y++;

        if (err < 0) {
            attron(COLOR_PAIR(CP_DISABLED));
            mvprintw(y, 4, "Relay error: %s", cc_err_name((uint32_t)-err));
            attroff(COLOR_PAIR(CP_DISABLED));
        } else if (count == 0) {
            attron(COLOR_PAIR(CP_DISABLED));
            mvprintw(y, 4, "(no guest VMs running — start a guest "
                     "via vibe_engine first)");
            attroff(COLOR_PAIR(CP_DISABLED));
        } else {
            attron(COLOR_PAIR(CP_DESC));
            mvprintw(y++, 4, "%-10s  %-10s  %-8s  %s",
                     "HANDLE", "STATE", "OS", "ARCH");
            attroff(COLOR_PAIR(CP_DESC));

            for (uint32_t i = 0; i < count; i++) {
                bool s = ((int)i == sel);
                if (s) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
                else   attron(COLOR_PAIR(CP_NORMAL));
                mvhline(y, 2, ' ', COLS - 4);
                mvprintw(y, 4, "%c 0x%08x  %-10s  %-8s  %s",
                         s ? '>' : ' ',
                         guests[i].guest_handle,
                         guest_state_name(guests[i].state),
                         guest_os_name(guests[i].os_type),
                         guest_arch_name(guests[i].arch));
                if (s) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
                else   attroff(COLOR_PAIR(CP_NORMAL));
                y++;
            }
        }

        refresh();

        int ch = getch();
        switch (ch) {
        case KEY_UP:
            if (sel > 0) sel--;
            break;
        case KEY_DOWN:
            if (count > 0 && sel < (int)count - 1) sel++;
            break;
        case 'r': case 'R':
            err = cc_do_list_guests(cc, guests, 16, &count);
            if (sel >= (int)count) sel = count > 0 ? (int)count - 1 : 0;
            break;
        case '\n': case KEY_ENTER:
            if (count > 0 && sel < (int)count)
                screen_guest_detail(cc, guests[sel].guest_handle);
            break;
        case 'q': case 'Q': case KEY_BACKSPACE: case 127:
            return;
        }
    }
}

/* ─── Main CC menu ───────────────────────────────────────────────────────── */

static int run_cc_main_menu(cc_client_t *cc)
{
#define N_CC_ITEMS 7
    static const char *cc_items[N_CC_ITEMS] = {
        "Guests          LIST_GUESTS / GUEST_STATUS / SNAPSHOT / RESTORE",
        "Devices         LIST_DEVICES / DEVICE_STATUS",
        "Framebuffer     ATTACH_FRAMEBUFFER + frame events",
        "Log Stream      LOG_STREAM (live drain)",
        "Polecats        LIST_POLECATS (agent pool status)",
        "Sessions        LIST + STATUS (session management)",
        "Disconnect & Quit",
    };

    int sel = 0;
    while (1) {
        clear();
        draw_header("CC Client");
        draw_hints("  \u2191\u2193 Navigate   Enter Select   Q Quit");
        draw_cc_statusbar(cc, LINES - 2);

        int y = 2;
        attron(A_BOLD);
        mvprintw(y++, 2, "agentOS CC Client  (session %u  fd %d)",
                 cc->session_id, cc->fd);
        attroff(A_BOLD);
        mvprintw(y++, 2, "All operations route through cc_pd "
                 "(cc_contract.h) — no direct PD calls.");
        y++;

        for (int i = 0; i < N_CC_ITEMS; i++) {
            bool s = (i == sel);
            if (s) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else if (i == N_CC_ITEMS - 1) attron(COLOR_PAIR(CP_DISABLED));
            else   attron(COLOR_PAIR(CP_NORMAL));
            mvhline(y, 2, ' ', COLS - 4);
            mvprintw(y, 2, " %c  %s", s ? '>' : ' ', cc_items[i]);
            if (s) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else if (i == N_CC_ITEMS - 1) attroff(COLOR_PAIR(CP_DISABLED));
            else   attroff(COLOR_PAIR(CP_NORMAL));
            y++;
        }

        refresh();

        int ch = getch();
        switch (ch) {
        case KEY_UP:   if (sel > 0)            sel--; break;
        case KEY_DOWN: if (sel < N_CC_ITEMS-1) sel++; break;
        case 'Q':      return 0;
        case 'q': case KEY_BACKSPACE: case 127: return 0;
        case '\n': case KEY_ENTER:
            switch (sel) {
            case 0: screen_guests(cc);    break;
            case 1: screen_devices(cc);   break;
            case 2: {
                /* Framebuffer: need a guest handle; use LIST_GUESTS first */
                cc_guest_info_t guests[16];
                uint32_t cnt = 0;
                cc_do_list_guests(cc, guests, 16, &cnt);
                if (cnt > 0)
                    screen_framebuffer(cc, guests[0].guest_handle);
                else
                    screen_framebuffer(cc, 0);
                break;
            }
            case 3: screen_log_stream(cc); break;
            case 4: screen_polecats(cc);   break;
            case 5: screen_sessions(cc);   break;
            case 6: return 0;
            }
            break;
        }
    }
#undef N_CC_ITEMS
}

/* ─── Post-boot: connect to CC PD and run management TUI ─────────────────── */

static int run_session_manager(void)
{
    bool sock_found = file_exists(CC_BRIDGE_SOCK);

    if (!sock_found) {
        /* Show a waiting screen — user may start agentOS and retry */
        while (1) {
            clear();
            draw_header("CC Connect");
            draw_hints("  r Retry   q Quit");

            int y = 3;
            attron(COLOR_PAIR(CP_DISABLED) | A_BOLD);
            mvprintw(y++, 4, "CC bridge socket not found:");
            attroff(COLOR_PAIR(CP_DISABLED) | A_BOLD);
            attron(COLOR_PAIR(CP_AVAIL));
            mvprintw(y++, 6, "%s", CC_BRIDGE_SOCK);
            attroff(COLOR_PAIR(CP_AVAIL));
            y++;
            mvprintw(y++, 4, "Start agentOS first, then re-run:");
            attron(COLOR_PAIR(CP_DESC));
            mvprintw(y++, 6, "agentctl           (use pre-boot menu)");
            mvprintw(y++, 6, "  or");
            mvprintw(y++, 6, "make demo TARGET_ARCH=aarch64");
            attroff(COLOR_PAIR(CP_DESC));
            y++;
            attron(COLOR_PAIR(CP_DESC));
            mvprintw(y++, 4, "CC protocol: MSG_CC_* opcodes via cc_contract.h");
            mvprintw(y++, 4, "Bridge:      cc_pd (agentOS) \u2192 cc_bridge.sock"
                     " (host)");
            attroff(COLOR_PAIR(CP_DESC));
            y++;
            mvprintw(y, 4, "Press 'r' to retry, 'q' to quit.");

            refresh();
            int ch = getch();
            if (ch == 'r' || ch == 'R') {
                sock_found = file_exists(CC_BRIDGE_SOCK);
                if (sock_found) break;
            } else if (ch == 'q' || ch == 'Q' || ch == 27) {
                return 0;
            }
        }
    }

    /* Open socket and establish CC session */
    cc_client_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.fd = cc_open_sock(CC_BRIDGE_SOCK);

    if (cc.fd < 0) {
        clear();
        draw_header("CC Connect");
        draw_hints("  Any key to exit");
        attron(COLOR_PAIR(CP_DISABLED) | A_BOLD);
        mvprintw(LINES / 2, 4, "connect(%s): %s", CC_BRIDGE_SOCK,
                 strerror(errno));
        attroff(COLOR_PAIR(CP_DISABLED) | A_BOLD);
        refresh();
        getch();
        return 1;
    }

    /* MSG_CC_CONNECT — establish session */
    int cerr = cc_do_connect(&cc);
    if (cerr != 0) {
        clear();
        draw_header("CC Connect");
        draw_hints("  Any key to exit");
        attron(COLOR_PAIR(CP_DISABLED) | A_BOLD);
        mvprintw(LINES / 2, 4,
                 "MSG_CC_CONNECT failed: %s (%d)",
                 cerr < 0 ? cc_err_name((uint32_t)-cerr) : "I/O error",
                 cerr);
        attroff(COLOR_PAIR(CP_DISABLED) | A_BOLD);
        refresh();
        getch();
        close(cc.fd);
        return 1;
    }

    /* Run the main CC management menu */
    run_cc_main_menu(&cc);

    /* MSG_CC_DISCONNECT — clean session teardown */
    cc_do_disconnect(&cc);
    close(cc.fd);
    return 0;
}

/* ─── Usage ──────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
            "agentctl v%s — agentOS Interactive Launcher & CC Client\n"
            "\n"
            "Usage:\n"
            "  %s              Pre-boot menu: select arch/board/guest, launch QEMU\n"
            "  %s -s           Post-boot CC client (connect to cc_pd bridge)\n"
            "  %s --sessions   Same as -s\n"
            "  %s --help       Show this help\n"
            "\n"
            "Run from the agentOS project root directory.\n"
            "\n"
            "Pre-boot menu:\n"
            "  Detects available QEMU binaries and guides you through architecture,\n"
            "  board, guest OS, and option selection. Exec()s the right qemu-system-*.\n"
            "\n"
            "Post-boot CC client (-s):\n"
            "  Connects to the CC PD bridge socket: %s\n"
            "  All operations use cc_contract.h MSG_CC_* opcodes exclusively.\n"
            "  Exercises the full API: guests, devices, framebuffer, log stream,\n"
            "  input injection, snapshot/restore, agent pool, session management.\n"
            "\n"
            "CC wire protocol:\n"
            "  Binary framing over Unix socket (see cc_wire_req_t / cc_wire_resp_t).\n"
            "  Maps directly to seL4 Microkit MR layout used by cc_pd.\n"
            "  Server-pushed events (frame-ready) use CC_WIRE_EVENT_MAGIC framing.\n"
            "\n"
            "Example:\n"
            "  make deps && make build TARGET_ARCH=aarch64\n"
            "  ./tools/agentctl/agentctl          # pre-boot\n"
            "  ./tools/agentctl/agentctl -s       # post-boot CC client\n",
            AGENTCTL_VERSION, prog, prog, prog, prog, CC_BRIDGE_SOCK);
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    bool sessions_mode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 ||
            strcmp(argv[i], "--sessions") == 0) {
            sessions_mode = true;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* Probe QEMU availability before entering ncurses */
    if (!sessions_mode)
        detect_qemu();

    /* Init ncurses */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    init_colors();

    int ret;
    if (sessions_mode)
        ret = run_session_manager();
    else
        ret = run_preboot_menu();

    endwin();
    return ret;
}
