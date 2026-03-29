#!/usr/bin/env bash
# agentOS CI test harness
# Usage: scripts/run-tests.sh [BOARD=qemu_virt_riscv64]
#
# Builds agentOS for the given board, boots it in QEMU, watches serial
# output for known success/failure strings, and exits 0 (PASS) or 1 (FAIL).

set -euo pipefail

BOARD="${BOARD:-${1:-qemu_virt_riscv64}}"
TIMEOUT=30

SUCCESS_STRINGS=("agentOS v0.1.0" "[event_bus] READY" "[controller] *** agentOS controller boot complete ***")
FAILURE_STRINGS=("Panic")

BUILD_IMAGE="build/${BOARD}/agentos.img"
TMPLOG=$(mktemp /tmp/agentos-qemu.XXXXXX)

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    rm -f "$TMPLOG"
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Step 1: Build
# ---------------------------------------------------------------------------
echo "[ci] Building agentOS for BOARD=${BOARD} ..."
if ! make BOARD="${BOARD}"; then
    echo ""
    echo "FAIL: build failed for BOARD=${BOARD}"
    exit 1
fi

if [[ ! -f "${BUILD_IMAGE}" ]]; then
    echo "FAIL: expected image not found at ${BUILD_IMAGE}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Step 2: Launch QEMU
# ---------------------------------------------------------------------------
echo "[ci] Booting QEMU for BOARD=${BOARD} ..."

case "${BOARD}" in
    qemu_virt_riscv64)
        qemu-system-riscv64 \
            -machine virt \
            -cpu rv64 \
            -m 2G \
            -nographic \
            -bios /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
            -kernel "${BUILD_IMAGE}" \
            > "$TMPLOG" 2>&1 &
        ;;
    qemu_virt_aarch64)
        qemu-system-aarch64 \
            -machine virt,virtualization=on \
            -cpu cortex-a57 \
            -m 2G \
            -nographic \
            -kernel "${BUILD_IMAGE}" \
            > "$TMPLOG" 2>&1 &
        ;;
    *)
        echo "FAIL: unknown BOARD=${BOARD} — add QEMU invocation to run-tests.sh"
        exit 1
        ;;
esac

QEMU_PID=$!
echo "[ci] QEMU PID=${QEMU_PID}, timeout=${TIMEOUT}s"

# ---------------------------------------------------------------------------
# Step 3: Watch serial output
# ---------------------------------------------------------------------------
declare -A seen_success=()
for s in "${SUCCESS_STRINGS[@]}"; do seen_success["$s"]=0; done

DEADLINE=$(( $(date +%s) + TIMEOUT ))
FAIL_REASON=""

while true; do
    NOW=$(date +%s)
    if (( NOW >= DEADLINE )); then
        FAIL_REASON="timeout after ${TIMEOUT}s"
        break
    fi

    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        FAIL_REASON="QEMU exited unexpectedly"
        break
    fi

    # Check failure strings
    for f in "${FAILURE_STRINGS[@]}"; do
        if grep -qF "$f" "$TMPLOG" 2>/dev/null; then
            FAIL_REASON="found failure string: \"${f}\""
            break 2
        fi
    done

    # Check success strings
    all_seen=true
    for s in "${SUCCESS_STRINGS[@]}"; do
        if [[ ${seen_success["$s"]} -eq 0 ]]; then
            if grep -qF "$s" "$TMPLOG" 2>/dev/null; then
                seen_success["$s"]=1
                echo "[ci] OK: \"${s}\""
            else
                all_seen=false
            fi
        fi
    done

    if $all_seen; then
        break
    fi

    sleep 0.5
done

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

# ---------------------------------------------------------------------------
# Step 4: Report
# ---------------------------------------------------------------------------
echo ""
echo "=== Serial output ==="
cat "$TMPLOG"
echo "====================="
echo ""

if [[ -n "$FAIL_REASON" ]]; then
    echo "FAIL [BOARD=${BOARD}]: ${FAIL_REASON}"
    exit 1
fi

echo "PASS [BOARD=${BOARD}]: all expected boot strings observed"
exit 0
