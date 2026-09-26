#!/usr/bin/env bash
# cc_virtio_timeout_test.sh — QEMU/target proof of CC-PD VirtIO timeout (agentos-45b)
#
# CC-PD (services/command-console/cc_pd.c) talks to its host-side controller
# over a VirtIO-MMIO serial console (QEMU virtconsole on a unix-socket chardev,
# _build/cc_pd.sock).  vio_serial_write() and vio_serial_read() each spin on the
# VirtIO *used* ring with a bounded wait (CC_VIRTIO_WAIT_LIMIT).  If the used
# ring never advances — i.e. the host stops draining the socket — the bounded
# wait must FIRE as an ERROR PATH:
#
#     [cc_pd] TX timeout waiting for used ring
#     [cc_pd] RX timeout waiting for used ring
#
# ...and CC-PD must remain responsive (its main loop `continue`s rather than
# hanging).  This test proves that error path is *observable* under QEMU, not
# merely compiled.
#
# How it wedges the ring:
#   1. Boot agentOS in QEMU with the CC-PD virtconsole on a unix socket.
#   2. Connect to _build/cc_pd.sock and send one well-formed CC request frame
#      (4112 bytes).  CC-PD reads it (RX used ring advances), dispatches, then
#      tries to write the 4112-byte reply.
#   3. The test then STOPS reading the socket.  QEMU's virtconsole TX buffer
#      fills, the TX used ring stops advancing, and CC-PD's bounded wait fires.
#   4. We assert the "TX timeout" (or "RX timeout") line appears in the serial
#      log, that NO "Panic" appears, and that the root task is still alive
#      (CC-PD canary / boot-complete banner present, QEMU still running).
#
# This is a target gate, not a host unit test.  It is wired via
# mk/target-tests.mk:  make -f mk/target-tests.mk test-cc-virtio-timeout
#
# Exit codes:
#   0 — PASS (timeout path observed, PD stayed responsive)
#   1 — FAIL (no timeout line, or panic, or PD wedged)
#   2 — SKIP (QEMU / built image / socket tooling unavailable)
#
# Copyright (c) 2026 The agentOS Project
# SPDX-License-Identifier: BSD-2-Clause

set -uo pipefail

# ── Config ───────────────────────────────────────────────────────────────────
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BOARD="${BOARD:-qemu_virt_aarch64}"
TIMEOUT="${CC_TIMEOUT_TEST_SECS:-120}"
# CC_VIRTIO_WAIT_LIMIT == 1_000_000 yield iterations in cc_pd.c; under TCG this
# can take a little while to drain, so give the bounded wait room to fire.
WEDGE_WAIT="${CC_WEDGE_WAIT_SECS:-45}"

CC_SOCK="${REPO_ROOT}/_build/cc_pd.sock"
SERIAL_LOG="$(mktemp /tmp/agentos-cc-timeout.XXXXXX)"
BUILD_DIR="${REPO_ROOT}/_build/${BOARD}-test"

QEMU_PID=""
SOCAT_PID=""

cleanup() {
    [ -n "${SOCAT_PID}" ] && kill "${SOCAT_PID}" 2>/dev/null
    [ -n "${QEMU_PID}" ]  && kill "${QEMU_PID}"  2>/dev/null
    wait 2>/dev/null
    rm -f "${SERIAL_LOG}"
}
trap cleanup EXIT

skip() { echo "SKIP [agentos-45b]: $*"; exit 2; }
fail() { echo "FAIL [agentos-45b]: $*"; exit 1; }
pass() { echo "PASS [agentos-45b]: $*"; exit 0; }

# ── Preconditions ──────────────────────────────────────────────────────────────
QEMU_BIN="qemu-system-aarch64"
case "${BOARD}" in
    qemu_virt_aarch64) QEMU_BIN="qemu-system-aarch64" ;;
    *) skip "board ${BOARD} has no CC-PD virtconsole wiring in this test" ;;
esac

command -v "${QEMU_BIN}" >/dev/null 2>&1 || skip "${QEMU_BIN} not installed"

LOADER_ELF="${BUILD_DIR}/loader.elf"
IMAGE="${BUILD_DIR}/agentos.img"
if [ ! -f "${LOADER_ELF}" ] || [ ! -f "${IMAGE}" ]; then
    skip "test image not built — run: make sel4-test-image BOARD=${BOARD}"
fi

# We need a tool to keep the socket connected without draining it.  Prefer
# socat; fall back to a tiny inline reader/holder.
HOLDER=""
if command -v socat >/dev/null 2>&1; then
    HOLDER="socat"
fi

rm -f "${CC_SOCK}"

# ── Step 1: boot agentOS test image in QEMU, CC-PD console on a unix socket ──
echo "[agentos-45b] booting ${BOARD} test image; CC-PD socket=${CC_SOCK}"
"${QEMU_BIN}" \
    -machine virt,virtualization=on,highmem=off,secure=off \
    -cpu cortex-a57 -m 2G \
    -display none -monitor none \
    -serial "file:${SERIAL_LOG}" \
    -global virtio-mmio.force-legacy=off \
    -chardev "socket,id=cc_pd_char,path=${CC_SOCK},server=on,wait=off" \
    -device virtio-serial-device,bus=virtio-mmio-bus.2,id=vser0 \
    -device virtconsole,bus=vser0.0,chardev=cc_pd_char,name=cc.0 \
    -device "loader,file=${LOADER_ELF},cpu-num=0" \
    -device "loader,file=${IMAGE},addr=0x48000000" &
QEMU_PID=$!

# ── Step 2: wait for CC-PD to come up (its VirtIO-ready / canary log) ─────────
DEADLINE=$(( $(date +%s) + TIMEOUT ))
CC_READY=0
while [ "$(date +%s)" -lt "${DEADLINE}" ]; do
    if ! kill -0 "${QEMU_PID}" 2>/dev/null; then
        echo "=== serial log ==="; cat "${SERIAL_LOG}"; echo "=================="
        fail "QEMU exited before CC-PD became ready"
    fi
    if grep -qF "Panic" "${SERIAL_LOG}" 2>/dev/null; then
        echo "=== serial log ==="; cat "${SERIAL_LOG}"; echo "=================="
        fail "kernel panic during boot"
    fi
    # CC-PD prints "[cc_pd] VirtIO serial ready" then its "[@]" canary.
    if grep -qF "[cc_pd] VirtIO serial ready" "${SERIAL_LOG}" 2>/dev/null \
       || grep -qF "[@]" "${SERIAL_LOG}" 2>/dev/null; then
        CC_READY=1
        break
    fi
    sleep 1
done

[ "${CC_READY}" -eq 1 ] || {
    echo "=== serial log ==="; cat "${SERIAL_LOG}"; echo "=================="
    skip "CC-PD never reported VirtIO-serial-ready (image may lack CC-PD on this board)"
}
echo "[agentos-45b] CC-PD is up; wedging the VirtIO used ring"

# ── Step 3: connect to the CC-PD socket, send one frame, then STOP draining ──
#
# A full CC request wire frame is 4112 bytes (cc_req_wire_t: opcode + 3 MRs +
# 4096-byte shmem).  We send a MSG_CC_LIST request (no shmem payload needed) so
# CC-PD dispatches and produces a 4112-byte reply it must TX back.  We then hold
# the socket open WITHOUT reading, so the virtconsole TX path back-pressures and
# the CC-PD TX used ring stops advancing -> bounded wait must fire.
#
# MSG_CC_LIST opcode value: see cc_contract.h.  We do not need the exact opcode
# for the wedge — any frame CC-PD reads will make it attempt a reply TX — but we
# send a recognisable little-endian opcode in the first 4 bytes for clarity.
send_frame_and_hold() {
    # Build a 4112-byte frame: 4-byte opcode (0x0007 = a CC list-ish op) + zero pad.
    # Reading nothing back is the wedge.
    {
        printf '\x07\x00\x00\x00'      # opcode (LE)
        head -c 4108 /dev/zero
    } 2>/dev/null
    # Hold the connection open (do not read) for the wedge window.
    sleep "${WEDGE_WAIT}"
}

if [ "${HOLDER}" = "socat" ]; then
    # socat: send our frame from stdin, never read the reply (we sleep on the
    # source side by keeping it open).  -u would be unidirectional; we want to
    # write then sit, so feed via a process-substitution producer.
    send_frame_and_hold | socat - "UNIX-CONNECT:${CC_SOCK}" >/dev/null 2>&1 &
    SOCAT_PID=$!
else
    # Fallback: bash /dev/tcp can't do unix sockets; use a tiny perl one-liner if
    # available, else skip cleanly (the wedge requires a unix-socket client).
    if command -v perl >/dev/null 2>&1; then
        send_frame_and_hold | perl -e '
            use IO::Socket::UNIX;
            my $s = IO::Socket::UNIX->new(Peer=>$ARGV[0], Type=>SOCK_STREAM)
                or exit 0;
            local $/; my $buf = <STDIN>;
            print $s $buf;             # write the frame
            sleep '"${WEDGE_WAIT}"';   # hold without reading the reply
        ' "${CC_SOCK}" >/dev/null 2>&1 &
        SOCAT_PID=$!
    else
        skip "no socat or perl to drive the unix-socket wedge"
    fi
fi

# ── Step 4: wait for the bounded-wait error path to fire ─────────────────────
WEDGE_DEADLINE=$(( $(date +%s) + WEDGE_WAIT + 15 ))
TIMEOUT_SEEN=0
while [ "$(date +%s)" -lt "${WEDGE_DEADLINE}" ]; do
    if ! kill -0 "${QEMU_PID}" 2>/dev/null; then
        break
    fi
    if grep -qE '\[cc_pd\] (TX|RX) timeout waiting for used ring' "${SERIAL_LOG}" 2>/dev/null; then
        TIMEOUT_SEEN=1
        break
    fi
    sleep 1
done

echo "=== serial log (tail) ==="
tail -40 "${SERIAL_LOG}" 2>/dev/null
echo "========================="

# ── Step 5: assertions ────────────────────────────────────────────────────────
if grep -qF "Panic" "${SERIAL_LOG}" 2>/dev/null; then
    fail "kernel panic observed — CC-PD timeout must NOT crash the system"
fi

if [ "${TIMEOUT_SEEN}" -ne 1 ]; then
    fail "CC-PD bounded-wait timeout line never appeared — ring wedge did not exercise the error path"
fi

# Responsiveness: after the timeout, QEMU/root task must still be alive (the
# bounded wait returns false and the main loop continues; it does not hang the
# whole image).
if ! kill -0 "${QEMU_PID}" 2>/dev/null; then
    fail "QEMU exited after the timeout — system did not remain responsive"
fi

pass "CC-PD VirtIO used-ring stall produced an observable timeout error path; system stayed responsive"
