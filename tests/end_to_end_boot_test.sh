#!/usr/bin/env bash
# agentOS End-to-End Boot Test
#
# Launches agentOS in QEMU and waits for the expected boot-completion markers
# in the serial output.  Designed for CI environments (no display, no KVM
# required — falls back to TCG softemu automatically).
#
# Exit codes:
#   0 — PASS (all required boot markers appeared within the timeout)
#   1 — FAIL (timeout, QEMU crash, or a required marker was not seen)
#   2 — SKIP (QEMU binary or kernel image not found; environment not set up)
#
# Environment variables (all optional):
#   AGENTOS_BOOT_TIMEOUT   seconds to wait for boot markers (default: 60)
#   AGENTOS_BOARD          override board selection (default: auto-detect)
#   AGENTOS_QEMU           override QEMU binary (default: auto-detect)
#   AGENTOS_IMAGE          override image path  (default: build/<board>/agentos.img)
#   AGENTOS_DEBUG          if set, echo all QEMU serial output to stdout

set -euo pipefail

# ── Colour helpers ─────────────────────────────────────────────────────────────

if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'
    BOLD='\033[1m';   RESET='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BOLD=''; RESET=''
fi

pass()  { printf "${GREEN}[PASS]${RESET} %s\n" "$*"; }
fail()  { printf "${RED}[FAIL]${RESET} %s\n" "$*"; }
skip()  { printf "${YELLOW}[SKIP]${RESET} %s\n" "$*"; }
info()  { printf "${BOLD}[INFO]${RESET} %s\n" "$*"; }
warn()  { printf "${YELLOW}[WARN]${RESET} %s\n" "$*"; }

# ── Configuration ─────────────────────────────────────────────────────────────

BOOT_TIMEOUT="${AGENTOS_BOOT_TIMEOUT:-60}"

# Locate repo root (parent of this script's directory).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Detect native arch (normalise arm64 → aarch64 to match seL4 conventions).
NATIVE_ARCH="$(uname -m | sed 's/arm64/aarch64/')"
UNAME_S="$(uname -s)"

# Board selection.
if [ -n "${AGENTOS_BOARD:-}" ]; then
    BOARD="${AGENTOS_BOARD}"
elif [ "${NATIVE_ARCH}" = "aarch64" ]; then
    BOARD="qemu_virt_aarch64"
elif [ "${NATIVE_ARCH}" = "x86_64" ]; then
    BOARD="x86_64_generic"
else
    BOARD="qemu_virt_riscv64"
fi

# QEMU binary selection.
if [ -n "${AGENTOS_QEMU:-}" ]; then
    QEMU_BIN="${AGENTOS_QEMU}"
elif [ "${BOARD}" = "qemu_virt_aarch64" ]; then
    QEMU_BIN="qemu-system-aarch64"
elif [ "${BOARD}" = "x86_64_generic" ]; then
    QEMU_BIN="qemu-system-x86_64"
else
    QEMU_BIN="qemu-system-riscv64"
fi

# Kernel image path.
if [ -n "${AGENTOS_IMAGE:-}" ]; then
    IMAGE="${AGENTOS_IMAGE}"
else
    IMAGE="${REPO_ROOT}/build/${BOARD}/agentos.img"
fi

# Temporary serial socket (matches the path used by the main Makefile).
SERIAL_SOCK="/tmp/agentos-e2e-boot-test-$$.sock"

# ── Boot-completion markers ───────────────────────────────────────────────────
#
# These strings are written to the serial console by the respective PDs when
# they finish initialising.  All of them must appear within BOOT_TIMEOUT seconds
# for the test to PASS.
#
# Source locations:
#   [event_bus]     kernel/agentos-root-task/src/event_bus.c:143
#   [net_server]    kernel/agentos-root-task/src/net_server.c:863
#   [vibe_engine]   kernel/agentos-root-task/src/vibe_engine.c:718 (starting) +
#                   the controller ack path
#   [controller]    kernel/agentos-root-task/src/monitor.c:572

REQUIRED_MARKERS=(
    "[event_bus] READY"
    "[net_server] READY"
    "[vibe_engine] VibeEngine PD starting"
    "[controller] EventBus: READY"
    "agentOS boot complete"
)

# ── Global state ──────────────────────────────────────────────────────────────

QEMU_PID=""
NC_PID=""
SERIAL_LOG="/tmp/agentos-e2e-serial-$$.log"

# ── Cleanup ───────────────────────────────────────────────────────────────────

cleanup() {
    local exit_code=$?
    if [ -n "${NC_PID}" ]; then
        kill "${NC_PID}" 2>/dev/null || true
        wait "${NC_PID}" 2>/dev/null || true
    fi
    if [ -n "${QEMU_PID}" ]; then
        # Send SIGTERM to the QEMU process group so virtio threads die cleanly.
        kill -TERM -- "-${QEMU_PID}" 2>/dev/null || kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    rm -f "${SERIAL_SOCK}" "${SERIAL_LOG}"
    exit "${exit_code}"
}
trap cleanup EXIT INT TERM

# ── Phase 1: Prerequisite checks ──────────────────────────────────────────────

printf "\n${BOLD}=== agentOS End-to-End Boot Test ===${RESET}\n\n"
info "Repo root : ${REPO_ROOT}"
info "Board     : ${BOARD}"
info "QEMU      : ${QEMU_BIN}"
info "Image     : ${IMAGE}"
info "Timeout   : ${BOOT_TIMEOUT}s"
printf "\n"

# Check QEMU is available.
if ! command -v "${QEMU_BIN}" >/dev/null 2>&1; then
    skip "QEMU binary '${QEMU_BIN}' not found — install QEMU to run this test"
    skip "  macOS: brew install qemu"
    skip "  Linux: apt install qemu-system-arm qemu-system-x86 qemu-system-misc"
    exit 2
fi
info "QEMU version: $(${QEMU_BIN} --version 2>/dev/null | head -1)"

# Check the kernel image exists.
if [ ! -f "${IMAGE}" ]; then
    skip "Kernel image not found: ${IMAGE}"
    skip "Build it first with: make build BOARD=${BOARD}"
    exit 2
fi
info "Image size: $(wc -c < "${IMAGE}") bytes"
printf "\n"

# ── Phase 2: Build QEMU flags ─────────────────────────────────────────────────

# Acceleration: HVF (macOS), KVM (Linux), TCG fallback.
ACCEL_FLAGS=""
if [ "${UNAME_S}" = "Darwin" ] && [ "${NATIVE_ARCH}" != "aarch64" ]; then
    # HVF works reliably on macOS/x86_64 for aarch64 / x86_64 QEMU guests.
    ACCEL_FLAGS="-accel hvf"
elif [ "${UNAME_S}" = "Linux" ] && [ -e /dev/kvm ]; then
    ACCEL_FLAGS="-enable-kvm"
fi
# If the chosen accel doesn't work QEMU will fall back to TCG automatically.

# Build the QEMU invocation matching the Makefile's NATIVE_QEMU_FLAGS.
case "${BOARD}" in
    qemu_virt_aarch64)
        CPU_FLAG="cortex-a53"
        if [ -n "${ACCEL_FLAGS}" ]; then CPU_FLAG="host"; fi
        QEMU_FLAGS=(
            -machine "virt,virtualization=on,highmem=off,secure=off"
            -cpu "${CPU_FLAG}" -m 2G
            -display none -monitor none
            -chardev "socket,id=char0,path=${SERIAL_SOCK},server=on,wait=off"
            -serial "chardev:char0"
            -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789"
            -device virtio-net-device,netdev=net0
            -device "loader,file=${IMAGE},addr=0x70000000,cpu-num=0"
        )
        if [ -n "${ACCEL_FLAGS}" ]; then
            QEMU_FLAGS+=( ${ACCEL_FLAGS} )
        fi
        ;;
    x86_64_generic)
        QEMU_FLAGS=(
            -machine q35 -cpu host -m 2G
            -display none -monitor none
            -serial "unix:${SERIAL_SOCK},server=on,nowait"
            -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789"
            -device e1000,netdev=net0
            -kernel "${IMAGE}"
        )
        if [ -n "${ACCEL_FLAGS}" ]; then
            QEMU_FLAGS+=( ${ACCEL_FLAGS} )
        fi
        ;;
    qemu_virt_riscv64)
        # Locate OpenSBI firmware.
        BIOS=""
        for candidate in \
            /opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
            /usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
            /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin; do
            if [ -f "${candidate}" ]; then BIOS="${candidate}"; break; fi
        done
        if [ -z "${BIOS}" ]; then
            warn "OpenSBI BIOS not found — RISC-V boot may fail"
        fi
        QEMU_FLAGS=(
            -machine virt -m 2G
            -display none -monitor none
            -serial "unix:${SERIAL_SOCK},server=on,nowait"
            -netdev "user,id=net0"
            -device virtio-net-device,netdev=net0
            -kernel "${IMAGE}"
        )
        if [ -n "${BIOS}" ]; then QEMU_FLAGS+=( -bios "${BIOS}" ); fi
        ;;
    *)
        fail "Unknown board '${BOARD}' — cannot build QEMU flags"
        exit 1
        ;;
esac

# ── Phase 3: Launch QEMU ──────────────────────────────────────────────────────

info "Launching: ${QEMU_BIN} ${QEMU_FLAGS[*]}"
printf "\n"

# Remove stale socket.
rm -f "${SERIAL_SOCK}"

# Start QEMU in its own process group so we can kill the whole group on exit.
setsid "${QEMU_BIN}" "${QEMU_FLAGS[@]}" >/dev/null 2>&1 &
QEMU_PID=$!
info "QEMU PID: ${QEMU_PID}"

# Wait for QEMU to create the serial socket (up to 10 s).
info "Waiting for serial socket to appear..."
SOCKET_WAIT=0
while [ ! -S "${SERIAL_SOCK}" ]; do
    if [ "${SOCKET_WAIT}" -ge 10 ]; then
        fail "Serial socket '${SERIAL_SOCK}' did not appear within 10 s"
        fail "QEMU may have crashed — check that the image is valid"
        exit 1
    fi
    if ! kill -0 "${QEMU_PID}" 2>/dev/null; then
        fail "QEMU process (PID ${QEMU_PID}) exited unexpectedly before socket appeared"
        exit 1
    fi
    sleep 1
    SOCKET_WAIT=$(( SOCKET_WAIT + 1 ))
done
info "Serial socket ready after ${SOCKET_WAIT}s"

# Connect to the serial socket and stream output to the log file.
# nc (netcat) with -U (Unix socket) is available on macOS and Linux.
touch "${SERIAL_LOG}"
if command -v nc >/dev/null 2>&1; then
    nc -U "${SERIAL_SOCK}" >> "${SERIAL_LOG}" 2>/dev/null &
    NC_PID=$!
else
    # Fallback: redirect QEMU serial to a named pipe via socat if available.
    if command -v socat >/dev/null 2>&1; then
        socat - "UNIX-CONNECT:${SERIAL_SOCK}" >> "${SERIAL_LOG}" 2>/dev/null &
        NC_PID=$!
    else
        fail "Neither 'nc' nor 'socat' found — cannot read serial output"
        exit 1
    fi
fi

# ── Phase 4: Poll for boot markers ───────────────────────────────────────────

info "Waiting up to ${BOOT_TIMEOUT}s for boot markers..."
printf "\n"

declare -A MARKER_SEEN
for m in "${REQUIRED_MARKERS[@]}"; do
    MARKER_SEEN["${m}"]=0
done

ELAPSED=0
ALL_FOUND=0

while [ "${ELAPSED}" -lt "${BOOT_TIMEOUT}" ]; do
    # Check if QEMU died unexpectedly.
    if ! kill -0 "${QEMU_PID}" 2>/dev/null; then
        fail "QEMU exited unexpectedly after ${ELAPSED}s"
        break
    fi

    # Scan the log for each not-yet-seen marker.
    ALL_FOUND=1
    for m in "${REQUIRED_MARKERS[@]}"; do
        if [ "${MARKER_SEEN["${m}"]}" -eq 0 ]; then
            if grep -qF "${m}" "${SERIAL_LOG}" 2>/dev/null; then
                MARKER_SEEN["${m}"]=1
                info "Found marker at ${ELAPSED}s: ${m}"
            else
                ALL_FOUND=0
            fi
        fi
    done

    if [ "${ALL_FOUND}" -eq 1 ]; then
        break
    fi

    sleep 1
    ELAPSED=$(( ELAPSED + 1 ))
done

# ── Phase 5: Report ───────────────────────────────────────────────────────────

printf "\n${BOLD}=== Boot Marker Summary ===${RESET}\n"
MARKERS_FOUND=0
MARKERS_MISSING=0
for m in "${REQUIRED_MARKERS[@]}"; do
    if [ "${MARKER_SEEN["${m}"]}" -eq 1 ]; then
        pass "  ${m}"
        MARKERS_FOUND=$(( MARKERS_FOUND + 1 ))
    else
        fail "  ${m}  (NOT SEEN)"
        MARKERS_MISSING=$(( MARKERS_MISSING + 1 ))
    fi
done

printf "\n"
info "Markers found  : ${MARKERS_FOUND} / $(( MARKERS_FOUND + MARKERS_MISSING ))"
info "Elapsed time   : ${ELAPSED}s / ${BOOT_TIMEOUT}s"

if [ "${MARKERS_MISSING}" -gt 0 ]; then
    printf "\n${BOLD}Last 40 lines of serial output:${RESET}\n"
    tail -40 "${SERIAL_LOG}" 2>/dev/null || true
fi

if [ -n "${AGENTOS_DEBUG:-}" ]; then
    printf "\n${BOLD}Full serial output:${RESET}\n"
    cat "${SERIAL_LOG}" 2>/dev/null || true
fi

printf "\n"
if [ "${ALL_FOUND}" -eq 1 ]; then
    pass "END-TO-END BOOT TEST: PASSED (all ${MARKERS_FOUND} markers found in ${ELAPSED}s)"
    exit 0
elif [ "${ELAPSED}" -ge "${BOOT_TIMEOUT}" ]; then
    fail "END-TO-END BOOT TEST: FAILED (timeout after ${BOOT_TIMEOUT}s; ${MARKERS_MISSING} marker(s) not seen)"
    exit 1
else
    fail "END-TO-END BOOT TEST: FAILED (QEMU exited after ${ELAPSED}s; ${MARKERS_MISSING} marker(s) not seen)"
    exit 1
fi
