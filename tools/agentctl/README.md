# agentctl — agentOS Interactive Launcher & CC Client

`agentctl` replaces the manual `make demo TARGET_ARCH=... BOARD=... GUEST_OS=...`
workflow with an interactive ncurses TUI that guides you from QEMU selection to
boot in a few keystrokes.

**Phase 5b:** The post-boot mode (`-s`) is now a full reference consumer of the
`cc_contract.h` API.  All post-boot operations route exclusively through `cc_pd`
via `MSG_CC_*` opcodes — no direct calls to service PDs.

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

## Post-boot CC client

```bash
./tools/agentctl/agentctl -s
./tools/agentctl/agentctl --sessions
```

Connects to the CC PD bridge socket at `build/cc_bridge.sock` and provides a
full management interface using `cc_contract.h` MSG_CC_* opcodes exclusively.
`cc_pd` relays every call to the appropriate service PD internally.

### CC client screens

| Screen | Opcodes exercised |
|--------|------------------|
| **Guests** | `MSG_CC_LIST_GUESTS`, `MSG_CC_GUEST_STATUS` |
| **Guest detail** | `MSG_CC_SNAPSHOT`, `MSG_CC_RESTORE`, `MSG_CC_ATTACH_FRAMEBUFFER`, `MSG_CC_SEND_INPUT` |
| **Devices** | `MSG_CC_LIST_DEVICES`, `MSG_CC_DEVICE_STATUS` |
| **Framebuffer** | `MSG_CC_ATTACH_FRAMEBUFFER` + `EVENT_FB_FRAME_READY` events |
| **Log Stream** | `MSG_CC_LOG_STREAM` (live drain of all log ring slots) |
| **Polecats** | `MSG_CC_LIST_POLECATS` (agent pool status) |
| **Sessions** | `MSG_CC_LIST`, `MSG_CC_STATUS` |

Session lifecycle: `MSG_CC_CONNECT` on entry, `MSG_CC_DISCONNECT` on exit.

### Wire protocol

Binary framing over Unix socket (`build/cc_bridge.sock`):

```
Request:  magic(u32) opcode(u32) mr[3](u32) shmem_len(u32) [shmem_data]
Response: magic(u32) mr[4](u32) shmem_len(u32) [shmem_data]
Event:    magic_event(u32) event_type(u32) data[32]
```

Maps directly to the seL4 Microkit MR layout used by `cc_pd`.
Server-pushed framebuffer frame-ready events use `CC_WIRE_EVENT_MAGIC` framing.

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
- `poll()` for non-blocking event receipt (framebuffer frame-ready events)

The pre-boot menu is a simple state machine over `SCREEN_ARCH → SCREEN_BOARD →
SCREEN_GUEST → SCREEN_OPTIONS → SCREEN_CONFIRM → launch`.

The post-boot CC client is a `cc_client_t` over a Unix socket that sends
`cc_wire_req_t` frames and receives `cc_wire_resp_t` / `cc_wire_event_t` frames,
mapping directly to the `cc_contract.h` MR layout.

All CC contract types (`cc_guest_info_t`, `cc_input_event_t`, etc.) and opcodes
(`MSG_CC_*`) are defined inline in `agentctl.c` — `cc_contract.h` depends on
seL4-specific headers unavailable on the host build.
