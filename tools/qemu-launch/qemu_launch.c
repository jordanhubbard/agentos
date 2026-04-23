/*
 * qemu-launch — agentOS Pre-boot QEMU Launcher
 *
 * Detects available QEMU binaries, presents an ncurses menu to select
 * architecture, board, guest OS, and launch options, then constructs
 * and exec()s the appropriate qemu-system-* command.
 *
 * Extracted from tools/agentctl/agentctl.c.  The post-boot CC client
 * lives in tools/agentctl-ng/agentctl_ng.c.
 *
 * Build:
 *   cc -o qemu-launch qemu_launch.c $(pkg-config --cflags --libs ncurses)
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ncurses.h>

/* ─── Constants ──────────────────────────────────────────────────────────── */

#define QEMU_LAUNCH_VERSION  "1.0.0"
#define MAX_CMD_LEN          4096

/* Color pair IDs */
#define CP_NORMAL    1
#define CP_HEADER    2
#define CP_SELECTED  3
#define CP_AVAIL     4
#define CP_DISABLED  5
#define CP_DESC      6
#define CP_HINT      7
#define CP_BORDER    8

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
    mvprintw(0, 2, "qemu-launch v%s  —  agentOS Launcher", QEMU_LAUNCH_VERSION);
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

static void draw_wrapped(const char *text, int y, int x, int w, int max_row)
{
    if (!text) return;
    attron(COLOR_PAIR(CP_DESC));
    const char *p = text;
    while (*p && y < max_row) {
        const char *nl = strchr(p, '\n');
        int seg = nl ? (int)(nl - p) : (int)strlen(p);
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

/* ─── Generic list menu ──────────────────────────────────────────────────── */

static int menu_screen(const char    *title,
                       const char   **labels,
                       int            n,
                       const bool    *avail,
                       int           *sel_io,
                       const char   *(*desc_fn)(int))
{
    int sel = *sel_io;
    if (sel < 0 || sel >= n) sel = 0;
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
        draw_hints("  ↑↓ Navigate   Enter Confirm   Backspace/q Back   Q Quit");

        int scroll = 0;
        if (sel >= inner_h) scroll = sel - inner_h + 1;

        for (int i = 0; i < n; i++) {
            if (i < scroll || (i - scroll) >= inner_h) continue;
            int row       = box_y + 1 + (i - scroll);
            bool is_sel   = (i == sel);
            bool is_avail = (!avail || avail[i]);

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

static const char *arch_desc(int i)  { return (i >= 0 && i < N_ARCHES) ? ARCHES[i].desc  : NULL; }
static const char *board_desc(int i) { return (i >= 0 && i < N_BOARDS) ? BOARDS[i].desc  : NULL; }
static const char *guest_desc(int i) { return (i >= 0 && i < N_GUESTS) ? GUESTS[i].desc  : NULL; }

/* ─── Options screen ─────────────────────────────────────────────────────── */

static int options_screen(cfg_t *cfg)
{
    bool has_kvm = kvm_available();
    int  sel     = 0;

#define N_OPT_ROWS 5

    while (1) {
        clear();
        draw_header("Options");
        draw_hints(
            "  ↑↓ Navigate   Space/Enter Toggle or Confirm   "
            "←→ Cycle memory   Backspace/q Back");

        int y = 3;
        mvprintw(y, 2, "Configure launch options:");
        y += 2;

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

        {
            bool _s = (sel == 4);
            if (_s) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            else    attron(COLOR_PAIR(CP_AVAIL)  | A_BOLD);
            mvhline(y, 2, ' ', COLS - 4);
            mvprintw(y, 2, " %c  Continue  →", _s ? '>' : ' ');
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
        int maxw = COLS - 6;
        const char *p = cmd;
        while (*p && y < LINES - 3) {
            int chunk = (int)strlen(p);
            if (chunk > maxw) chunk = maxw;
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
        case '\n': case KEY_ENTER: return 1;
        case KEY_BACKSPACE: case 127: case '\b': case 'q': return -1;
        case 'Q': return -2;
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
                for (int i = 0; i < N_BOARDS; i++) {
                    if (strcmp(BOARDS[i].arch, ARCHES[cfg.arch].id) == 0) {
                        cfg.board = i;
                        break;
                    }
                }
                cfg.guest = 0;
                screen = SCREEN_BOARD;
            }
            break;
        }

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

        case SCREEN_GUEST: {
            const char *labels[N_GUESTS];
            bool        avail[N_GUESTS];
            for (int i = 0; i < N_GUESTS; i++) {
                labels[i] = GUESTS[i].label;
                if (i == 0) {
                    avail[i] = true;
                } else if (strcmp(ARCHES[cfg.arch].id, "aarch64") != 0) {
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

        case SCREEN_OPTIONS:
            ret = options_screen(&cfg);
            if (ret == -2) return 0;
            if (ret >  0)  screen = SCREEN_CONFIRM;
            if (ret == -1) screen = SCREEN_GUEST;
            break;

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

    char cmd[MAX_CMD_LEN];
    build_qemu_cmd(&cfg, cmd, sizeof(cmd));

    endwin();

    if (cfg.dry_run) {
        printf("\n[qemu-launch] Dry run — QEMU command:\n\n  %s\n\n", cmd);
        return 0;
    }

    printf("\n[qemu-launch] Launching agentOS...\n");
    printf("  %s\n\n", cmd);
    printf("  (Ctrl-A X to quit QEMU)\n\n");
    fflush(stdout);

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

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("qemu-launch v%s — agentOS Pre-boot QEMU Launcher\n\n"
                   "Usage:\n"
                   "  %s           Interactive menu: select arch/board/guest, launch QEMU\n"
                   "  %s --help    Show this help\n\n"
                   "Run from the agentOS project root directory.\n"
                   "For post-boot CC client: use tools/agentctl-ng/agentctl-ng\n",
                   QEMU_LAUNCH_VERSION, argv[0], argv[0]);
            return 0;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return 1;
    }

    detect_qemu();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    init_colors();

    int ret = run_preboot_menu();
    endwin();
    return ret;
}
