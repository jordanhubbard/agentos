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
IMAGE="${AGENTOS_IMAGE:-${ROOT_DIR}/kernel/agentos-root-task/build-riscv/agentos.img}"
TIMEOUT="${AGENTOS_TIMEOUT:-30}"
LOG="${AGENTOS_LOG:-/tmp/agentos-ci.log}"

# BIOS auto-detection
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
echo "  Image  : ${IMAGE}"
echo "  BIOS   : ${BIOS}"
echo "  Timeout: ${TIMEOUT}s"
echo "  Log    : ${LOG}"
echo ""

if [[ ! -f "${IMAGE}" ]]; then
    echo "ERROR: kernel image not found: ${IMAGE}"
    echo "       Run 'make build' first."
    exit 1
fi

if [[ ! -f "${BIOS}" ]]; then
    echo "ERROR: OpenSBI BIOS not found: ${BIOS}"
    echo "       Run 'make deps' first."
    exit 1
fi

if ! command -v qemu-system-riscv64 &>/dev/null; then
    echo "ERROR: qemu-system-riscv64 not found. Run 'make deps' first."
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
    qemu-system-riscv64 \
        -machine virt \
        -cpu rv64 \
        -m 2G \
        -nographic \
        -bios "${BIOS}" \
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
check "OpenSBI firmware loaded"                  "OpenSBI"
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
