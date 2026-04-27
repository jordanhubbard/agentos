#!/usr/bin/env bash
# agentOS End-to-End Full Integration Test Suite
#
# Brings up agentOS + guest VMs in QEMU, SSHes into guests, exercises every
# IPC contract defined in contracts/, and verifies the cap_policy ring-1
# enforcement rejects ring-0 escalation attempts.
#
# Exit codes:
#   0 — PASS (all tests passed)
#   1 — FAIL (one or more tests failed)
#   2 — SKIP (prerequisites not met — QEMU, images, or SSH tools missing)
#
# Environment variables (all optional):
#   E2E_TIMEOUT         seconds to wait for guest SSH (default: 120)
#   E2E_CC_PORT         agentOS CC bridge port forwarded by QEMU (default: 8789)
#   E2E_SSH_PORT        host port forwarded to guest SSH (default: 2222)
#   E2E_BOARD           override board selection (default: auto-detect)
#   E2E_QEMU            override QEMU binary (default: auto-detect)
#   E2E_IMAGE           override agentos image (default: build/<board>/agentos.img)
#   E2E_GUEST_OS        guest OS to test: freebsd|ubuntu-amd64|ubuntu-arm64|nixos
#                       (default: freebsd; set to "all" to loop all available images)
#   E2E_FREEBSD_IMG     FreeBSD disk image for slot 0 (default: guest-images/freebsd.img)
#   E2E_SSH_KEY         path to ED25519 private key (default: tests/e2e/id_ed25519)
#   E2E_DEBUG           if set, echo all serial output to stdout
#   E2E_SKIP_SSH        if set, skip SSH-based tests (useful in restricted envs)
#   E2E_SKIP_BRIDGE     if set, skip HTTP bridge/CC API tests

set -uo pipefail

# ── Colour helpers ─────────────────────────────────────────────────────────────

if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'
    BOLD='\033[1m';   RESET='\033[0m';    CYAN='\033[0;36m'
else
    RED=''; GREEN=''; YELLOW=''; BOLD=''; RESET=''; CYAN=''
fi

pass()    { printf "${GREEN}[PASS]${RESET} %s\n"   "$*"; TESTS_PASSED=$(( TESTS_PASSED + 1 )); }
fail()    { printf "${RED}[FAIL]${RESET} %s\n"     "$*"; TESTS_FAILED=$(( TESTS_FAILED + 1 )); }
skip()    { printf "${YELLOW}[SKIP]${RESET} %s\n"  "$*"; TESTS_SKIPPED=$(( TESTS_SKIPPED + 1 )); }
info()    { printf "${BOLD}[INFO]${RESET} %s\n"    "$*"; }
warn()    { printf "${YELLOW}[WARN]${RESET} %s\n"  "$*"; }
section() { printf "\n${CYAN}━━━ %s ━━━${RESET}\n" "$*"; }

# ── Configuration ──────────────────────────────────────────────────────────────

E2E_TIMEOUT="${E2E_TIMEOUT:-120}"
E2E_CC_PORT="${E2E_CC_PORT:-8789}"
E2E_SSH_PORT="${E2E_SSH_PORT:-2222}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

NATIVE_ARCH="$(uname -m | sed 's/arm64/aarch64/')"
UNAME_S="$(uname -s)"

# Board selection
if [ -n "${E2E_BOARD:-}" ]; then
    BOARD="${E2E_BOARD}"
elif [ "${NATIVE_ARCH}" = "aarch64" ]; then
    BOARD="qemu_virt_aarch64"
elif [ "${NATIVE_ARCH}" = "x86_64" ]; then
    BOARD="x86_64_generic"
else
    BOARD="qemu_virt_riscv64"
fi

# QEMU binary
if [ -n "${E2E_QEMU:-}" ]; then
    QEMU_BIN="${E2E_QEMU}"
elif [ "${BOARD}" = "qemu_virt_aarch64" ]; then
    QEMU_BIN="qemu-system-aarch64"
elif [ "${BOARD}" = "x86_64_generic" ]; then
    QEMU_BIN="qemu-system-x86_64"
else
    QEMU_BIN="qemu-system-riscv64"
fi

# Images
E2E_IMAGE="${E2E_IMAGE:-${REPO_ROOT}/build/${BOARD}/agentos.img}"
E2E_LOADER_ELF="${E2E_LOADER_ELF:-${REPO_ROOT}/build/${BOARD}/loader.elf}"
E2E_FREEBSD_IMG="${E2E_FREEBSD_IMG:-${REPO_ROOT}/guest-images/freebsd.img}"
E2E_SSH_KEY="${E2E_SSH_KEY:-${SCRIPT_DIR}/id_ed25519}"

# ── Guest OS selection ─────────────────────────────────────────────────────────
# E2E_GUEST_OS selects which guest disk image to boot.
# When set to "all", the suite re-runs for each available image in sequence.
E2E_GUEST_OS="${E2E_GUEST_OS:-freebsd}"

# Resolve the guest image path and vmm_type for the selected OS.
# Sub-scripts read E2E_GUEST_VMM_TYPE to parameterise cc_post JSON payloads.
resolve_guest_os() {
    local gos="$1"
    case "${gos}" in
        freebsd)
            E2E_GUEST_IMG="${E2E_FREEBSD_IMG}"
            E2E_GUEST_VMM_TYPE="freebsd"
            E2E_GUEST_BOOT_MARKER="login:"
            ;;
        ubuntu-amd64)
            E2E_GUEST_IMG="${E2E_GUEST_IMG:-${REPO_ROOT}/guest-images/ubuntu-amd64.img}"
            E2E_GUEST_VMM_TYPE="linux"
            E2E_GUEST_BOOT_MARKER="login:"
            ;;
        ubuntu-arm64)
            E2E_GUEST_IMG="${E2E_GUEST_IMG:-${REPO_ROOT}/guest-images/ubuntu-arm64.img}"
            E2E_GUEST_VMM_TYPE="linux"
            E2E_GUEST_BOOT_MARKER="login:"
            ;;
        nixos)
            E2E_GUEST_IMG="${E2E_GUEST_IMG:-${REPO_ROOT}/guest-images/nixos.img}"
            E2E_GUEST_VMM_TYPE="linux"
            E2E_GUEST_BOOT_MARKER="<<< NixOS Stage 2"
            ;;
        freebsd15)
            E2E_GUEST_IMG="${E2E_GUEST_IMG:-${REPO_ROOT}/guest-images/freebsd15-amd64.img}"
            E2E_GUEST_VMM_TYPE="freebsd"
            E2E_GUEST_BOOT_MARKER="login:"
            ;;
        *)
            printf "Unknown E2E_GUEST_OS '%s'. Valid: freebsd ubuntu-amd64 ubuntu-arm64 nixos freebsd15 all\n" "${gos}" >&2
            exit 2
            ;;
    esac
    export E2E_GUEST_VMM_TYPE E2E_GUEST_BOOT_MARKER E2E_GUEST_IMG
}

resolve_guest_os "${E2E_GUEST_OS}"

# Temporary files
SERIAL_SOCK="/tmp/agentos-e2e-$$.sock"
SERIAL_LOG="/tmp/agentos-e2e-serial-$$.log"

# Counters
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

# Process tracking
QEMU_PID=""
SERIAL_NC_PID=""

# ── SSH helper ─────────────────────────────────────────────────────────────────

# Run a command inside the guest via SSH.
# Usage: guest_ssh <command>
guest_ssh() {
    ssh -i "${E2E_SSH_KEY}" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=10 \
        -o BatchMode=yes \
        -p "${E2E_SSH_PORT}" \
        root@localhost "$@" 2>/dev/null
}

# ── CC bridge helper ───────────────────────────────────────────────────────────

CC_BASE="http://localhost:${E2E_CC_PORT}"

# Send a request to the agentOS CC bridge.
# Usage: cc_call <endpoint> [json-body]
cc_call() {
    local endpoint="$1"
    local data="${2:-{}}"
    curl -sf --max-time 5 \
        -X POST "${CC_BASE}/api/agentos/cc/${endpoint}" \
        -H "Content-Type: application/json" \
        -d "${data}" 2>/dev/null
}

# GET variant for read-only queries.
cc_get() {
    local endpoint="$1"
    curl -sf --max-time 5 \
        "${CC_BASE}/api/agentos/cc/${endpoint}" 2>/dev/null
}

# Check if the CC bridge is responding.
cc_available() {
    curl -sf --max-time 3 "${CC_BASE}/api/agentos/cc/status" >/dev/null 2>&1
}

# ── Cleanup ────────────────────────────────────────────────────────────────────

cleanup() {
    local code=$?
    if [ -n "${SERIAL_NC_PID}" ]; then
        kill "${SERIAL_NC_PID}" 2>/dev/null || true
        wait "${SERIAL_NC_PID}" 2>/dev/null || true
    fi
    if [ -n "${QEMU_PID}" ]; then
        kill -TERM -- "-${QEMU_PID}" 2>/dev/null || kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    rm -f "${SERIAL_SOCK}" "${SERIAL_LOG}"
    exit "${code}"
}
trap cleanup EXIT INT TERM

# ── Helper: wait for a marker in the serial log ───────────────────────────────

wait_for_marker() {
    local marker="$1"
    local timeout="$2"
    local elapsed=0
    while [ "${elapsed}" -lt "${timeout}" ]; do
        if grep -qF "${marker}" "${SERIAL_LOG}" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "${QEMU_PID}" 2>/dev/null; then
            return 1
        fi
        sleep 1
        elapsed=$(( elapsed + 1 ))
    done
    return 1
}

# ── Helper: generate SSH key if missing ───────────────────────────────────────

ensure_ssh_key() {
    if [ ! -f "${E2E_SSH_KEY}" ]; then
        info "Generating test SSH key: ${E2E_SSH_KEY}"
        ssh-keygen -t ed25519 -N "" -f "${E2E_SSH_KEY}" -C "agentos-e2e-test" >/dev/null 2>&1
        if [ $? -ne 0 ]; then
            warn "ssh-keygen failed — SSH tests will be skipped"
            return 1
        fi
        chmod 600 "${E2E_SSH_KEY}"
        info "SSH key generated: ${E2E_SSH_KEY}"
        info "Public key: ${E2E_SSH_KEY}.pub"
        info "NOTE: The guest image must have this key in /root/.ssh/authorized_keys"
        info "      for SSH tests to pass.  Bake it in when building guest images."
    fi
    return 0
}

# ── Prerequisite checks ────────────────────────────────────────────────────────

printf "\n${BOLD}══════════════════════════════════════════${RESET}\n"
printf "${BOLD}  agentOS End-to-End Integration Test Suite${RESET}\n"
printf "${BOLD}══════════════════════════════════════════${RESET}\n\n"

info "Repo root  : ${REPO_ROOT}"
info "Board      : ${BOARD}"
info "QEMU       : ${QEMU_BIN}"
info "Image      : ${E2E_IMAGE}"
info "Loader ELF : ${E2E_LOADER_ELF}"
info "FreeBSD img: ${E2E_FREEBSD_IMG}"
info "CC port    : ${E2E_CC_PORT}"
info "SSH port   : ${E2E_SSH_PORT}"
info "SSH key    : ${E2E_SSH_KEY}"
info "Timeout    : ${E2E_TIMEOUT}s"
printf "\n"

HAVE_QEMU=1
HAVE_IMAGE=1
HAVE_GUEST_IMG=1
HAVE_SSH_TOOLS=1
HAVE_CURL=1

if ! command -v "${QEMU_BIN}" >/dev/null 2>&1; then
    skip "QEMU binary '${QEMU_BIN}' not found"
    HAVE_QEMU=0
fi

if [ ! -f "${E2E_IMAGE}" ]; then
    skip "agentOS image not found: ${E2E_IMAGE} (run 'make build BOARD=${BOARD}')"
    HAVE_IMAGE=0
fi

if [ ! -f "${E2E_GUEST_IMG}" ]; then
    skip "Guest image for '${E2E_GUEST_OS}' not found: ${E2E_GUEST_IMG}"
    skip "  Run: tools/bootstrap-guest.sh ${E2E_GUEST_OS}"
    HAVE_GUEST_IMG=0
fi

if ! command -v ssh >/dev/null 2>&1 || ! command -v ssh-keygen >/dev/null 2>&1; then
    skip "OpenSSH client tools not found"
    HAVE_SSH_TOOLS=0
fi

if ! command -v curl >/dev/null 2>&1; then
    warn "curl not found — CC bridge tests will be skipped"
    HAVE_CURL=0
fi

if [ "${HAVE_QEMU}" -eq 0 ] || [ "${HAVE_IMAGE}" -eq 0 ]; then
    printf "\n${YELLOW}[SKIP]${RESET} Prerequisites not met — skipping all E2E tests\n"
    exit 2
fi

if [ "${HAVE_SSH_TOOLS}" -eq 1 ]; then
    ensure_ssh_key || HAVE_SSH_TOOLS=0
fi

# ── QEMU flags ─────────────────────────────────────────────────────────────────

# Acceleration
ACCEL_FLAGS=""
if [ "${UNAME_S}" = "Darwin" ] && [ "${NATIVE_ARCH}" = "x86_64" ]; then
    ACCEL_FLAGS="-accel hvf"
elif [ "${UNAME_S}" = "Linux" ] && [ -e /dev/kvm ]; then
    ACCEL_FLAGS="-enable-kvm"
fi

# Build hostfwd list: CC bridge + SSH to guest VM slot 0
HOSTFWD="hostfwd=tcp:127.0.0.1:${E2E_CC_PORT}-:${E2E_CC_PORT}"
HOSTFWD="${HOSTFWD},hostfwd=tcp:127.0.0.1:${E2E_SSH_PORT}-:22"

# Attach guest image as a block device if available
GUEST_BLOCK_FLAGS=()
if [ "${HAVE_GUEST_IMG}" -eq 1 ]; then
    GUEST_BLOCK_FLAGS+=(
        -drive "file=${E2E_GUEST_IMG},if=virtio,format=raw,readonly=off"
    )
fi

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
            -netdev "user,id=net0,${HOSTFWD}"
            -device virtio-net-device,netdev=net0
            -device "loader,file=${E2E_LOADER_ELF},cpu-num=0"
            -device "loader,file=${E2E_IMAGE},addr=0x48000000"
            "${GUEST_BLOCK_FLAGS[@]+"${GUEST_BLOCK_FLAGS[@]}"}"
        )
        if [ -n "${ACCEL_FLAGS}" ]; then QEMU_FLAGS+=( ${ACCEL_FLAGS} ); fi
        ;;
    x86_64_generic)
        QEMU_FLAGS=(
            -machine q35 -cpu host -m 2G
            -display none -monitor none
            -serial "unix:${SERIAL_SOCK},server=on,nowait"
            -netdev "user,id=net0,${HOSTFWD}"
            -device e1000,netdev=net0
            -kernel "${E2E_IMAGE}"
            "${GUEST_BLOCK_FLAGS[@]+"${GUEST_BLOCK_FLAGS[@]}"}"
        )
        if [ -n "${ACCEL_FLAGS}" ]; then QEMU_FLAGS+=( ${ACCEL_FLAGS} ); fi
        ;;
    *)
        fail "Unsupported board '${BOARD}' for E2E tests"
        exit 1
        ;;
esac

# ── Launch QEMU ────────────────────────────────────────────────────────────────

section "Phase 1: Starting QEMU"
info "Command: ${QEMU_BIN} ${QEMU_FLAGS[*]}"
printf "\n"

rm -f "${SERIAL_SOCK}"
touch "${SERIAL_LOG}"

setsid "${QEMU_BIN}" "${QEMU_FLAGS[@]}" >/dev/null 2>&1 &
QEMU_PID=$!
info "QEMU PID: ${QEMU_PID}"

# Wait for the serial socket
info "Waiting for serial socket..."
SOCK_WAIT=0
while [ ! -S "${SERIAL_SOCK}" ]; do
    if [ "${SOCK_WAIT}" -ge 15 ]; then
        fail "Serial socket did not appear within 15s"
        exit 1
    fi
    if ! kill -0 "${QEMU_PID}" 2>/dev/null; then
        fail "QEMU exited unexpectedly before socket appeared"
        exit 1
    fi
    sleep 1
    SOCK_WAIT=$(( SOCK_WAIT + 1 ))
done
info "Serial socket ready (${SOCK_WAIT}s)"

# Connect to the serial socket
if command -v nc >/dev/null 2>&1; then
    nc -U "${SERIAL_SOCK}" >> "${SERIAL_LOG}" 2>/dev/null &
    SERIAL_NC_PID=$!
elif command -v socat >/dev/null 2>&1; then
    socat - "UNIX-CONNECT:${SERIAL_SOCK}" >> "${SERIAL_LOG}" 2>/dev/null &
    SERIAL_NC_PID=$!
else
    fail "Neither nc nor socat available — cannot capture serial output"
    exit 2
fi

# ── Wait for agentOS boot ──────────────────────────────────────────────────────

section "Phase 2: Waiting for agentOS boot"

AGENTOS_BOOT_MARKERS=(
    "[controller] EventBus: READY"
    "agentOS boot complete"
)

BOOT_TIMEOUT=60
info "Waiting up to ${BOOT_TIMEOUT}s for agentOS boot markers..."

AGENTOS_BOOTED=1
for marker in "${AGENTOS_BOOT_MARKERS[@]}"; do
    if wait_for_marker "${marker}" "${BOOT_TIMEOUT}"; then
        info "  ✓ ${marker}"
    else
        fail "  ✗ ${marker} not seen within ${BOOT_TIMEOUT}s"
        AGENTOS_BOOTED=0
    fi
done

if [ "${AGENTOS_BOOTED}" -eq 0 ]; then
    fail "agentOS did not boot — cannot run guest tests"
    printf "\nLast 40 lines of serial output:\n"
    tail -40 "${SERIAL_LOG}" 2>/dev/null || true
    exit 1
fi

pass "agentOS boot complete"

# ── Wait for guest VM (slot 0) ────────────────────────────────────────────────

GUEST_BOOTED=0
if [ "${HAVE_GUEST_IMG}" -eq 1 ]; then
    section "Phase 3: Waiting for ${E2E_GUEST_OS} VM (slot 0)"
    info "Polling serial for boot marker '${E2E_GUEST_BOOT_MARKER}' (timeout ${E2E_TIMEOUT}s)..."

    if wait_for_marker "${E2E_GUEST_BOOT_MARKER}" "${E2E_TIMEOUT}"; then
        info "${E2E_GUEST_OS} boot marker found"
        GUEST_BOOTED=1
        pass "${E2E_GUEST_OS} VM (slot 0) booted and reached login prompt"
    else
        fail "${E2E_GUEST_OS} VM did not reach boot marker within ${E2E_TIMEOUT}s"
        printf "\nLast 60 lines of serial output:\n"
        tail -60 "${SERIAL_LOG}" 2>/dev/null || true
    fi

    if [ "${GUEST_BOOTED}" -eq 1 ] && [ "${HAVE_SSH_TOOLS}" -eq 1 ] && \
       [ -z "${E2E_SKIP_SSH:-}" ]; then
        info "Waiting for SSH on port ${E2E_SSH_PORT}..."
        SSH_WAIT=0
        SSH_AVAILABLE=0
        while [ "${SSH_WAIT}" -lt 30 ]; do
            if guest_ssh true 2>/dev/null; then
                SSH_AVAILABLE=1
                break
            fi
            sleep 2
            SSH_WAIT=$(( SSH_WAIT + 2 ))
        done
        if [ "${SSH_AVAILABLE}" -eq 1 ]; then
            info "SSH available after ${SSH_WAIT}s"
        else
            warn "SSH not available on port ${E2E_SSH_PORT} — SSH tests will be skipped"
            HAVE_SSH_TOOLS=0
        fi
    fi
else
    warn "Guest image not found for ${E2E_GUEST_OS} — skipping guest VM tests"
fi

# Backward-compat alias so sub-scripts that still reference FREEBSD_BOOTED work
FREEBSD_BOOTED="${GUEST_BOOTED}"

# ── Check CC bridge availability ───────────────────────────────────────────────

BRIDGE_AVAILABLE=0
if [ "${HAVE_CURL}" -eq 1 ] && [ -z "${E2E_SKIP_BRIDGE:-}" ]; then
    info "Checking CC bridge at ${CC_BASE}..."
    if cc_available; then
        BRIDGE_AVAILABLE=1
        info "CC bridge is responding"
    else
        warn "CC bridge not responding at ${CC_BASE} — skipping CC API tests"
    fi
fi

# ── Source and run test modules ────────────────────────────────────────────────

# Export helpers and config for sub-scripts
export E2E_TIMEOUT E2E_CC_PORT E2E_SSH_PORT E2E_SSH_KEY
export CC_BASE SCRIPT_DIR REPO_ROOT SERIAL_LOG
export E2E_GUEST_OS E2E_GUEST_VMM_TYPE E2E_GUEST_BOOT_MARKER E2E_GUEST_IMG
export GUEST_BOOTED FREEBSD_BOOTED BRIDGE_AVAILABLE HAVE_SSH_TOOLS HAVE_CURL
export -f pass fail skip info warn section guest_ssh cc_call cc_get

run_test_module() {
    local script="$1"
    local name="$2"
    if [ -f "${SCRIPT_DIR}/${script}" ]; then
        section "${name}"
        # Run in subshell to isolate failures; inherit counters via file
        bash "${SCRIPT_DIR}/${script}" 2>&1
        local rc=$?
        if [ "${rc}" -eq 2 ]; then
            skip "${name}: prerequisites not met"
        fi
    else
        warn "Test script not found: ${SCRIPT_DIR}/${script}"
    fi
}

run_test_module "test_guest_lifecycle.sh"   "Guest Contract Lifecycle"
run_test_module "test_device_contracts.sh"  "Device Contracts (serial/net/block/USB)"
run_test_module "test_framebuffer.sh"       "Framebuffer Contract"
run_test_module "test_vibeos.sh"            "VibeOS Contract (create/boot/status/snapshot/destroy)"
run_test_module "test_vibeos_restore.sh"    "VibeOS Restore (snapshot → restore → verify)"
run_test_module "test_vibeos_migrate.sh"    "VibeOS Migrate (live migration between slots)"
run_test_module "test_cc_contract.sh"       "Command-and-Control Contract"
run_test_module "test_cap_policy.sh"        "Cap Policy (ring-1 enforcement)"

# SSH suite is special — it runs inside the guest
if [ "${FREEBSD_BOOTED}" -eq 1 ] && [ "${HAVE_SSH_TOOLS}" -eq 1 ]; then
    section "Guest SSH Command Suite"
    bash "${SCRIPT_DIR}/suite_common.sh" 2>&1
fi

# ── Summary ────────────────────────────────────────────────────────────────────

printf "\n${BOLD}══════════════════════════════════════════${RESET}\n"
printf "${BOLD}  E2E Test Summary${RESET}\n"
printf "${BOLD}══════════════════════════════════════════${RESET}\n"
printf "  ${GREEN}PASSED${RESET}:  %d\n" "${TESTS_PASSED}"
printf "  ${RED}FAILED${RESET}:  %d\n"  "${TESTS_FAILED}"
printf "  ${YELLOW}SKIPPED${RESET}: %d\n" "${TESTS_SKIPPED}"
printf "\n"

if [ -n "${E2E_DEBUG:-}" ]; then
    printf "Full serial log:\n"
    cat "${SERIAL_LOG}" 2>/dev/null || true
fi

if [ "${TESTS_FAILED}" -gt 0 ]; then
    printf "${RED}[FAIL]${RESET} E2E suite: ${TESTS_FAILED} test(s) failed\n\n"
    exit 1
elif [ "${TESTS_PASSED}" -eq 0 ]; then
    printf "${YELLOW}[SKIP]${RESET} E2E suite: no tests ran (prerequisites not met)\n\n"
    exit 2
else
    printf "${GREEN}[PASS]${RESET} E2E suite: all ${TESTS_PASSED} test(s) passed\n\n"
    exit 0
fi
