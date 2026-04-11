#!/usr/bin/env bash
# Boot agentOS in QEMU and verify the boot banner appears within 30 seconds.
set -euo pipefail

IMAGE="${1:-build/qemu_virt_riscv64/agentos.img}"
BANNER="${2:-agentOS v0.1.0}"
TIMEOUT="${3:-30}"

if [[ ! -f "$IMAGE" ]]; then
    echo "FAIL: image not found at $IMAGE"
    exit 1
fi

echo "[boot-test] Image  : $IMAGE"
echo "[boot-test] Banner : $BANNER"
echo "[boot-test] Timeout: ${TIMEOUT}s"

output=$(timeout "$TIMEOUT" qemu-system-riscv64 \
    -machine virt \
    -cpu rv64 \
    -nographic \
    -m 512M \
    -bios /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
    -kernel "$IMAGE" \
    -serial stdio 2>&1 || true)

if echo "$output" | grep -qF "$BANNER"; then
    echo "PASS: boot banner found: \"$BANNER\""
    exit 0
else
    echo "FAIL: boot banner not found within ${TIMEOUT}s"
    echo "--- serial output ---"
    echo "$output"
    echo "---------------------"
    exit 1
fi
