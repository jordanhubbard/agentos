# agentctl — agentOS Interactive Launcher

`agentctl` replaces the manual `make demo TARGET_ARCH=... BOARD=... GUEST_OS=...`
workflow with an interactive ncurses TUI that guides you from QEMU selection to
boot in a few keystrokes.

## Quick start

```bash
# From the project root:
make deps           # install build dependencies
make agentctl       # build the agentctl binary

# Launch the pre-boot menu:
./tools/agentctl/agentctl
```

The menu auto-detects which `qemu-system-*` binaries are installed and shows
only the architectures you can actually boot.

## Pre-boot menu

Navigates through four screens:

| Screen | What you pick |
|--------|--------------|
| Architecture | riscv64 / aarch64 / x86_64 |
| Board | qemu_virt_riscv64 / qemu_virt_aarch64 / x86_64_generic |
| Guest OS | agentOS only / +Linux VMM / +FreeBSD VMM |
| Options | Memory, KVM, GDB stub, Dry-run toggle |

After the Confirm screen, agentctl `exec()`s the appropriate `qemu-system-*`
command directly — no subshell, no extra processes.

### Keyboard

| Key | Action |
|-----|--------|
| `↑` / `↓` | Navigate items |
| `Enter` / `Space` | Select / toggle |
| `←` / `→` | Cycle memory size |
| `Backspace` / `q` | Go back |
| `Q` | Quit immediately |

### Guest OS availability

VMM guest options are only shown as selectable if:
- The target architecture is **aarch64** (VMM requires EL2 hypervisor mode)
- The corresponding ELF exists in `build/<board>/` (e.g. `linux_vmm.elf`)

Build the VMM first:

```bash
make build TARGET_ARCH=aarch64 GUEST_OS=linux
make build TARGET_ARCH=aarch64 GUEST_OS=freebsd
```

### Dry-run mode

Toggle **Dry run** in the Options screen to print the full QEMU command without
executing it. Useful for debugging or copying the command to a script.

### GDB stub

Toggle **GDB stub** to append `-s -S` to the QEMU command. QEMU will:
- Listen for a GDB remote stub on `tcp::1234`
- Halt at boot (won't proceed until GDB connects and issues `continue`)

Connect with: `gdb-multiarch` or `lldb` → `gdbremote connect :1234`

## Post-boot session manager

```bash
./tools/agentctl/agentctl -s
./tools/agentctl/agentctl --sessions
```

Connects to the QEMU monitor socket at `build/qemu_monitor.sock` and provides
an interface to manage agentOS console sessions via the `console_mux` PD.

**Note:** The full IPC bridge requires the controller serial port to be
forwarded. If the socket is not found, agentctl shows instructions for
starting agentOS first.

### Controller commands (over 2nd serial port)

| Command | Effect |
|---------|--------|
| `list` | List active console sessions (OP_CONSOLE_LIST) |
| `attach <pd_id>` | Attach to PD console (OP_CONSOLE_ATTACH) |
| `detach` | Detach current session (OP_CONSOLE_DETACH) |
| `mode <0\|1\|2>` | Set console mode (OP_CONSOLE_MODE) |
| `scroll <n>` | Scroll n lines (OP_CONSOLE_SCROLL) |
| `status` | Show console_mux status (OP_CONSOLE_STATUS) |
| `broadcast <msg>` | Send message to all sessions |

## Building

### Linux (apt)

```bash
sudo apt-get install libncurses-dev
make -C tools/agentctl
```

### macOS (brew)

```bash
brew install ncurses
make -C tools/agentctl
```

### From the project root

```bash
make agentctl
```

This calls `$(MAKE) -C tools/agentctl` automatically.

## Architecture

`agentctl` is a single self-contained C file (`agentctl.c`) with no dependencies
beyond ncurses. It uses:
- `initscr()` / `endwin()` for ncurses lifecycle
- `execvp()` to replace itself with the QEMU process (no fork needed)
- `stat()` to check for QEMU binaries, `/dev/kvm`, and guest ELFs

The pre-boot menu is a simple state machine over `SCREEN_ARCH → SCREEN_BOARD →
SCREEN_GUEST → SCREEN_OPTIONS → SCREEN_CONFIRM → launch`.
