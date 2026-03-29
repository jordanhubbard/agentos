#!/usr/bin/env bash
# ci-test.sh — agentOS CI test harness
#
# Boots agentOS in QEMU, captures serial output, checks for expected
# test markers, exits 0 on PASS or 1 on FAIL.
#
# Usage:
#   ./scripts/ci-test.sh [IMAGE] [BIOS]
#
# Env overrides:
#   AGENTOS_IMAGE   path to agentos.img (default: auto-detected)
#   AGENTOS_BIOS    path to opensbi firmware (default: auto-detected)
#   AGENTOS_TIMEOUT boot timeout in seconds (default: 30)
#   AGENTOS_LOG     path to save serial output (default: /tmp/agentos-ci.log)

set -euo pipefail

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Architecture: riscv64 (default) or aarch64 (set AGENTOS_ARCH=aarch64)
ARCH="${AGENTOS_ARCH:-riscv64}"

if [[ "${ARCH}" == "aarch64" ]]; then
    IMAGE="${AGENTOS_IMAGE:-${ROOT_DIR}/kernel/agentos-root-task/build-aarch64/agentos.img}"
    QEMU_BIN="qemu-system-aarch64"
    # virtualization=on → EL2 (required by seL4 hypervisor config in qemu_virt_aarch64 SDK)
    QEMU_ARGS="-machine virt,virtualization=on -cpu cortex-a57 -m 2G -nographic"
    BIOS=""  # AArch64 virt machine has built-in firmware
else
    IMAGE="${AGENTOS_IMAGE:-${ROOT_DIR}/kernel/agentos-root-task/build-riscv/agentos.img}"
    QEMU_BIN="qemu-system-riscv64"
    # BIOS auto-detection for RISC-V
    if [[ -z "${AGENTOS_BIOS:-}" ]]; then
        for candidate in \
            /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
            /usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
            "$(find /opt/homebrew /usr/local -name 'opensbi-riscv64-generic-fw_dynamic.bin' 2>/dev/null | head -1)"; do
            if [[ -f "$candidate" ]]; then
                BIOS="$candidate"
                break
            fi
        done
    fi
    BIOS="${AGENTOS_BIOS:-${BIOS:-/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin}}"
    QEMU_ARGS="-machine virt -cpu rv64 -m 2G -nographic -bios ${BIOS}"
fi

TIMEOUT="${AGENTOS_TIMEOUT:-30}"
LOG="${AGENTOS_LOG:-/tmp/agentos-ci-${ARCH}.log}"

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
PASS=0
FAIL=0

ok()   { echo "  ✓ $*"; }
fail() { echo "  ✗ $*"; FAIL=$((FAIL + 1)); }
info() { echo "    $*"; }

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║      agentOS CI Test Harness                     ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""
echo "  Arch   : ${ARCH}"
echo "  Image  : ${IMAGE}"
if [[ "${ARCH}" != "aarch64" ]]; then
echo "  BIOS   : ${BIOS}"
fi
echo "  Timeout: ${TIMEOUT}s"
echo "  Log    : ${LOG}"
echo ""

if [[ ! -f "${IMAGE}" ]]; then
    echo "ERROR: kernel image not found: ${IMAGE}"
    echo "       Run 'make build' first."
    exit 1
fi

if [[ "${ARCH}" != "aarch64" && ! -f "${BIOS}" ]]; then
    echo "ERROR: OpenSBI BIOS not found: ${BIOS}"
    echo "       Run 'make deps' first."
    exit 1
fi

if ! command -v "${QEMU_BIN}" &>/dev/null; then
    echo "ERROR: ${QEMU_BIN} not found. Run 'make deps' first."
    exit 1
fi

# ---------------------------------------------------------------------------
# Boot QEMU, capture serial output with timeout
# ---------------------------------------------------------------------------
echo "── Booting agentOS in QEMU ────────────────────────"
echo ""

# Run QEMU in background, capturing all serial output.
# -chardev + -serial stdio routes debug output to stdout.
# timeout kills QEMU after ${TIMEOUT}s regardless of what happens.
timeout "${TIMEOUT}" \
    ${QEMU_BIN} \
        ${QEMU_ARGS} \
        -kernel "${IMAGE}" \
    2>&1 | tee "${LOG}" || true

echo ""
echo "── Analyzing output ───────────────────────────────"
echo ""

# ---------------------------------------------------------------------------
# Test suite — check serial output for expected markers
# ---------------------------------------------------------------------------
check() {
    local desc="$1"
    local pattern="$2"
    if grep -qF "${pattern}" "${LOG}"; then
        ok "${desc}"
        PASS=$((PASS + 1))
    else
        fail "${desc}"
        info "Expected: '${pattern}'"
    fi
}

# Boot sequence
if [[ "${ARCH}" != "aarch64" ]]; then
check "OpenSBI firmware loaded"                  "OpenSBI"
fi
check "agentOS banner printed"                   "agentOS v0.1.0-alpha"
check "Protection domains listed"                "controller"

# EventBus lifecycle
check "EventBus initializing"                    "[event_bus] Initializing..."
check "EventBus READY"                           "[event_bus] READY"

# Controller → EventBus handshake
check "Controller woke EventBus"                 "[controller] Waking EventBus via PPC..."
check "Controller confirmed EventBus READY"      "[controller] EventBus: READY"

# InitAgent → EventBus subscription
check "InitAgent subscribing to EventBus"        "[init_agent] Subscribing to EventBus..."
check "EventBus subscription OK"                 "[init_agent] EventBus subscription: OK"

# Controller boot complete
check "Controller boot complete"                 "[controller] *** agentOS controller boot complete ***"
check "Controller ready for agents"              "[controller] Ready for agents."

# InitAgent alive
check "InitAgent event loop running (ALIVE)"     "[init_agent] Entering event loop. agentOS is ALIVE."

# ---------------------------------------------------------------------------
# Results
# ---------------------------------------------------------------------------
TOTAL=$((PASS + FAIL))
echo ""
echo "── Results ─────────────────────────────────────────"
echo ""
printf "  Passed: %d / %d\n" "${PASS}" "${TOTAL}"

if [[ ${FAIL} -eq 0 ]]; then
    echo ""
    echo "  ✅ PASS — all ${PASS} checks passed"
    echo ""
    exit 0
else
    echo ""
    echo "  ❌ FAIL — ${FAIL} check(s) failed"
    echo ""
    echo "  Full serial log: ${LOG}"
    echo ""
    exit 1
fi
