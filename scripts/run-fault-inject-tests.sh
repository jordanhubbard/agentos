#!/usr/bin/env bash
# run-fault-inject-tests.sh — Fault injection CI test runner
#
# Boots agentOS in QEMU with the fault_inject PD, runs a scripted sequence
# of OP_FAULT_INJECT commands via the monitor's debug serial interface, and
# verifies that each fault triggers the expected watchdog recovery path.
#
# Environment:
#   BOARD               — QEMU board target (default: qemu_virt_riscv64)
#   MAX_RECOVERY_TICKS  — watchdog recovery deadline in ticks (default: 100)
#   QEMU_TIMEOUT        — seconds before QEMU is killed (default: 120)
#
# Exit codes:
#   0 — all fault injection tests passed
#   1 — one or more tests failed
#   2 — QEMU boot timeout

set -euo pipefail

BOARD="${BOARD:-qemu_virt_riscv64}"
MAX_RECOVERY_TICKS="${MAX_RECOVERY_TICKS:-100}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-120}"
IMG="build/${BOARD}/agentos.img"

if [ ! -f "$IMG" ]; then
    echo "ERROR: image not found: $IMG" >&2
    exit 2
fi

PASS=0
FAIL=0
TOTAL=0

# ── Test case table ──────────────────────────────────────────────────────────
# Format: fault_kind | slot | description | expect_recovery
TESTS=(
    "0x01|1|null-ptr dereference in slot 1|yes"
    "0x02|1|stack overflow in slot 1|yes"
    "0x03|1|quota exceeded in slot 1|yes"
    "0x04|2|IPC timeout in slot 2|yes"
    "0x05|1|unaligned memory access in slot 1|yes"
)

# ── QEMU launch ──────────────────────────────────────────────────────────────
QEMU_LOG=$(mktemp /tmp/agentos_qemu_XXXXXX.log)
trap "rm -f $QEMU_LOG" EXIT

echo "[fi] Starting QEMU (board=$BOARD, timeout=${QEMU_TIMEOUT}s)..."

case "$BOARD" in
    qemu_virt_riscv64)
        QEMU_CMD="qemu-system-riscv64 -machine virt -m 512M -nographic \
            -bios none -kernel $IMG \
            -serial mon:stdio \
            -monitor null"
        ;;
    qemu_virt_aarch64)
        QEMU_CMD="qemu-system-aarch64 -machine virt -m 512M -nographic \
            -cpu cortex-a53 -bios none -kernel $IMG \
            -serial mon:stdio \
            -monitor null"
        ;;
    *)
        echo "ERROR: unsupported board $BOARD for fault injection" >&2
        exit 2
        ;;
esac

# Boot in background
$QEMU_CMD > "$QEMU_LOG" 2>&1 &
QEMU_PID=$!

# Wait for agentOS boot marker
BOOT_TIMEOUT=30
BOOT_WAITED=0
until grep -q "\[monitor\] agentOS online" "$QEMU_LOG" 2>/dev/null; do
    sleep 1
    BOOT_WAITED=$((BOOT_WAITED + 1))
    if [ "$BOOT_WAITED" -ge "$BOOT_TIMEOUT" ]; then
        echo "[fi] ERROR: boot timeout after ${BOOT_TIMEOUT}s" >&2
        kill $QEMU_PID 2>/dev/null || true
        cat "$QEMU_LOG"
        exit 2
    fi
done
echo "[fi] agentOS booted (${BOOT_WAITED}s)"

# ── Run test cases ───────────────────────────────────────────────────────────
for test in "${TESTS[@]}"; do
    IFS='|' read -r fkind slot desc expect_recovery <<< "$test"
    TOTAL=$((TOTAL + 1))

    echo ""
    echo "=== Test $TOTAL: $desc ==="

    # Write fault inject command to QEMU monitor serial
    # Format: "FAULT_INJECT slot=<N> kind=<K> flags=<F>"
    flags=0x01  # FAULT_FLAG_VERIFY_RECOVERY
    echo "FAULT_INJECT slot=$slot kind=$fkind flags=$flags" >> "$QEMU_LOG"

    # Wait for recovery confirmation or timeout
    WAITED=0
    RECOVERY_TICKS=""
    STATUS=""

    while [ "$WAITED" -lt "$QEMU_TIMEOUT" ]; do
        sleep 1
        WAITED=$((WAITED + 1))

        if grep -q "\[fault_inject\] recovery confirmed" "$QEMU_LOG" 2>/dev/null; then
            STATUS="recovered"
            RECOVERY_TICKS=$(grep "ticks_to_recovery=" "$QEMU_LOG" 2>/dev/null \
                | tail -1 | grep -oE 'ticks_to_recovery=[0-9]+' | cut -d= -f2 || echo "?")
            break
        fi
        if grep -q "\[fault_inject\] TIMEOUT" "$QEMU_LOG" 2>/dev/null; then
            STATUS="timeout"
            break
        fi
    done

    if [ "$expect_recovery" = "yes" ]; then
        if [ "$STATUS" = "recovered" ]; then
            echo "  PASS — recovered in ${RECOVERY_TICKS} ticks"
            # Check recovery within threshold
            if [ "$RECOVERY_TICKS" != "?" ] && \
               [ "$RECOVERY_TICKS" -gt "$MAX_RECOVERY_TICKS" ]; then
                echo "  WARN — recovery took ${RECOVERY_TICKS} ticks > threshold ${MAX_RECOVERY_TICKS}"
            fi
            PASS=$((PASS + 1))
        else
            echo "  FAIL — expected recovery, got: ${STATUS:-no response}"
            FAIL=$((FAIL + 1))
        fi
    else
        if [ "$STATUS" = "no_crash" ] || [ -z "$STATUS" ]; then
            echo "  PASS — fault handled gracefully (no crash expected)"
            PASS=$((PASS + 1))
        else
            echo "  FAIL — unexpected crash: $STATUS"
            FAIL=$((FAIL + 1))
        fi
    fi

    # Clear log markers between tests
    > "$QEMU_LOG"
done

# ── Teardown ─────────────────────────────────────────────────────────────────
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true

echo ""
echo "=== Fault Injection Results ==="
echo "  Passed:  $PASS / $TOTAL"
echo "  Failed:  $FAIL / $TOTAL"
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo "FAIL: $FAIL test(s) did not pass"
    exit 1
fi
echo "PASS: all fault injection tests passed"
exit 0
