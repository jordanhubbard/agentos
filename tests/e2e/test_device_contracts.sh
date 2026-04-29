#!/usr/bin/env bash
# agentOS E2E — Device Contract Tests
#
# Tests: serial_contract.h, net_contract.h, block_contract.h, usb_contract.h
#
# serial_contract: serial console accessible, getty running, login prompt seen
# net_contract:    guest gets IP, SSH port reachable from host
# block_contract:  root filesystem mounted read-write, disk I/O round-trip
# usb_contract:    USB device enumeration visible inside guest (lsusb or camcontrol)
#
# Exit codes:
#   0 — PASS
#   1 — FAIL
#   2 — SKIP (prerequisites not met)

set -uo pipefail

if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'
    BOLD='\033[1m';   RESET='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BOLD=''; RESET=''
fi

PASS=0; FAIL=0; SKIP=0
pass() { printf "  ${GREEN}[PASS]${RESET} %s\n" "$*"; PASS=$(( PASS + 1 )); }
fail() { printf "  ${RED}[FAIL]${RESET} %s\n"   "$*"; FAIL=$(( FAIL + 1 )); }
skip() { printf "  ${YELLOW}[SKIP]${RESET} %s\n" "$*"; SKIP=$(( SKIP + 1 )); }

CC_PORT="${E2E_CC_PORT:-8789}"
CC_BASE="http://localhost:${CC_PORT}"
SSH_PORT="${E2E_SSH_PORT:-2222}"
SSH_KEY="${E2E_SSH_KEY:-$(dirname "$0")/id_ed25519}"
SSH_USER="${E2E_SSH_USER:-root}"
SSH_CMD_TIMEOUT="${E2E_SSH_CMD_TIMEOUT:-20}"
SERIAL_LOG="${SERIAL_LOG:-/dev/null}"
BRIDGE_AVAIL="${BRIDGE_AVAILABLE:-0}"
HAVE_SSH="${HAVE_SSH_TOOLS:-0}"

cc_post() {
    curl -sf --max-time 5 \
        -X POST "${CC_BASE}/api/agentos/cc/$1" \
        -H "Content-Type: application/json" \
        -d "${2:-{}}" 2>/dev/null
}

cc_get() {
    curl -sf --max-time 5 "${CC_BASE}/api/agentos/cc/$1" 2>/dev/null
}

ok_field() {
    printf '%s' "$1" | grep -q '"ok":true'
}

run_bounded() {
    local seconds="$1"
    shift
    if command -v timeout >/dev/null 2>&1; then
        timeout "${seconds}" "$@"
    elif command -v gtimeout >/dev/null 2>&1; then
        gtimeout "${seconds}" "$@"
    else
        "$@"
    fi
}

gssh() {
    run_bounded "${SSH_CMD_TIMEOUT}" ssh -i "${SSH_KEY}" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=10 \
        -o BatchMode=yes \
        -p "${SSH_PORT}" \
        "${SSH_USER}@localhost" "$@" 2>/dev/null
}

# ══════════════════════════════════════════════════════════════════════════════
# serial_contract.h tests
# ══════════════════════════════════════════════════════════════════════════════

printf "  ${BOLD}serial_contract.h${RESET}\n"

# Test 1: serial console shows login prompt (verified via serial log)
if grep -qF "login:" "${SERIAL_LOG}" 2>/dev/null; then
    pass "serial: login prompt visible on console (getty running)"
elif grep -qiE "login:|getty" "${SERIAL_LOG}" 2>/dev/null; then
    pass "serial: getty/login activity on serial console"
elif [ "${E2E_GUEST_OS:-}" = "ubuntu-arm64" ] || [ "${E2E_GUEST_OS:-}" = "ubuntu-amd64" ]; then
    if grep -qF "Linux version" "${SERIAL_LOG}" 2>/dev/null && \
       grep -qF "systemd[1]:" "${SERIAL_LOG}" 2>/dev/null; then
        pass "serial: Ubuntu console emitted kernel and systemd boot logs"
    else
        skip "serial: Ubuntu boot output not captured by this harness"
    fi
else
    fail "serial: no login prompt in serial log"
fi

# Test 2: MSG_SERIAL_STATUS via CC bridge
if [ "${BRIDGE_AVAIL}" = "1" ]; then
    RESP="$(cc_post "serial/status" '{"client_slot":0}')"
    if ok_field "${RESP}"; then
        BAUD="$(printf '%s' "${RESP}" | grep -o '"baud":[0-9]*' | grep -o '[0-9]*')"
        pass "MSG_SERIAL_STATUS: baud=${BAUD:-?}"
    else
        fail "MSG_SERIAL_STATUS: request failed (${RESP:-<empty>})"
    fi
else
    skip "MSG_SERIAL_STATUS: CC bridge not available"
fi

# Test 3: MSG_SERIAL_WRITE — write to serial console via bridge
if [ "${BRIDGE_AVAIL}" = "1" ]; then
    RESP="$(cc_post "serial/write" '{"client_slot":0,"data":"e2e-test-ping\n"}')"
    if ok_field "${RESP}"; then
        pass "MSG_SERIAL_WRITE: wrote to serial console"
    else
        fail "MSG_SERIAL_WRITE: failed (${RESP:-<empty>})"
    fi
else
    skip "MSG_SERIAL_WRITE: CC bridge not available"
fi

# ══════════════════════════════════════════════════════════════════════════════
# net_contract.h tests
# ══════════════════════════════════════════════════════════════════════════════

printf "\n  ${BOLD}net_contract.h${RESET}\n"

# Test 4: MSG_NET_DEV_STATUS via CC bridge
if [ "${BRIDGE_AVAIL}" = "1" ]; then
    RESP="$(cc_post "net/status" '{"handle":0}')"
    if ok_field "${RESP}"; then
        LINK="$(printf '%s' "${RESP}" | grep -o '"link":[0-9]*' | grep -o '[0-9]*')"
        RX="$(printf '%s'  "${RESP}" | grep -o '"rx_pkts":[0-9]*' | grep -o '[0-9]*')"
        TX="$(printf '%s'  "${RESP}" | grep -o '"tx_pkts":[0-9]*' | grep -o '[0-9]*')"
        pass "MSG_NET_DEV_STATUS: link=${LINK:-?} rx=${RX:-?} tx=${TX:-?}"
    else
        fail "MSG_NET_DEV_STATUS: failed (${RESP:-<empty>})"
    fi
else
    skip "MSG_NET_DEV_STATUS: CC bridge not available"
fi

# Test 5: SSH port reachable from host (proves guest got an IP and route exists)
if timeout 5 bash -c "echo > /dev/tcp/localhost/${SSH_PORT}" 2>/dev/null; then
    pass "net: SSH port ${SSH_PORT} reachable from host"
elif nc -z -w 3 localhost "${SSH_PORT}" 2>/dev/null; then
    pass "net: SSH port ${SSH_PORT} reachable from host (nc)"
else
    fail "net: SSH port ${SSH_PORT} not reachable from host"
fi

# Test 6: guest has a network interface with an IP (via SSH)
if [ "${HAVE_SSH}" = "1" ] && gssh true 2>/dev/null; then
    # Try both ifconfig (BSD) and ip addr (Linux)
    IF_OUT="$(gssh ifconfig 2>/dev/null || gssh ip addr 2>/dev/null)"
    if printf '%s' "${IF_OUT}" | grep -qE 'inet [0-9]|inet addr:[0-9]'; then
        IP="$(printf '%s' "${IF_OUT}" | grep -oE 'inet [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1 | awk '{print $2}')"
        pass "net: guest has IP address: ${IP:-<detected>}"
    else
        fail "net: no inet address found in ifconfig/ip output"
    fi
else
    skip "net: guest IP check via SSH (SSH not available)"
fi

# ══════════════════════════════════════════════════════════════════════════════
# block_contract.h tests
# ══════════════════════════════════════════════════════════════════════════════

printf "\n  ${BOLD}block_contract.h${RESET}\n"

# Test 7: root filesystem mounted read-write (via SSH)
if [ "${HAVE_SSH}" = "1" ] && gssh true 2>/dev/null; then
    # Check root filesystem is mounted rw
    MOUNT_OUT="$(gssh mount 2>/dev/null)"
    if printf '%s' "${MOUNT_OUT}" | grep -qE '/ .*rw|/ on .* type .*rw'; then
        pass "block: root filesystem mounted read-write"
    elif gssh test -w / 2>/dev/null; then
        pass "block: root filesystem is writable"
    else
        fail "block: root filesystem not mounted read-write"
    fi

    # Test 8: disk I/O round-trip — write a file and read it back
    TMPF="/tmp/block_e2e_$$.txt"
    MAGIC="agentos_e2e_block_$(date +%s 2>/dev/null || echo 42)"
    gssh "printf '%s' '${MAGIC}' > ${TMPF}" 2>/dev/null
    READ_BACK="$(gssh "cat ${TMPF}" 2>/dev/null)"
    gssh "rm -f ${TMPF}" 2>/dev/null
    if [ "${READ_BACK}" = "${MAGIC}" ]; then
        pass "block: disk I/O round-trip (write→read verified)"
    else
        fail "block: disk I/O mismatch (wrote '${MAGIC}', read '${READ_BACK:-<empty>}')"
    fi

    # Test 9: df shows block device with reasonable size
    DF_OUT="$(gssh df / 2>/dev/null)"
    if printf '%s' "${DF_OUT}" | grep -qE '^/dev/'; then
        DEV="$(printf '%s' "${DF_OUT}" | grep -E '^/dev/' | awk '{print $1}')"
        pass "block: root device identified: ${DEV:-<unknown>}"
    else
        skip "block: df output doesn't show /dev/ device (may be virtual FS)"
    fi
else
    skip "block: filesystem checks via SSH (SSH not available)"
fi

# Test 10: MSG_BLOCK_STATUS via CC bridge
if [ "${BRIDGE_AVAIL}" = "1" ]; then
    RESP="$(cc_post "block/status" '{"handle":0}')"
    if ok_field "${RESP}"; then
        SECTORS="$(printf '%s' "${RESP}" | grep -o '"sectors":[0-9]*' | grep -o '[0-9]*')"
        SECT_SZ="$(printf '%s' "${RESP}" | grep -o '"sector_sz":[0-9]*' | grep -o '[0-9]*')"
        pass "MSG_BLOCK_STATUS: sectors=${SECTORS:-?} sector_sz=${SECT_SZ:-?}"
    else
        fail "MSG_BLOCK_STATUS: failed (${RESP:-<empty>})"
    fi
else
    skip "MSG_BLOCK_STATUS: CC bridge not available"
fi

# ══════════════════════════════════════════════════════════════════════════════
# usb_contract.h tests
# ══════════════════════════════════════════════════════════════════════════════

printf "\n  ${BOLD}usb_contract.h${RESET}\n"

# Test 11: MSG_USB_LIST via CC bridge
if [ "${BRIDGE_AVAIL}" = "1" ]; then
    RESP="$(cc_get "usb/list")"
    if ok_field "${RESP}"; then
        COUNT="$(printf '%s' "${RESP}" | grep -o '"count":[0-9]*' | grep -o '[0-9]*')"
        pass "MSG_USB_LIST: ${COUNT:-?} USB device(s) enumerated"
    else
        fail "MSG_USB_LIST: failed (${RESP:-<empty>})"
    fi
else
    skip "MSG_USB_LIST: CC bridge not available"
fi

# Test 12: USB enumeration visible inside guest (via SSH)
if [ "${HAVE_SSH}" = "1" ] && gssh true 2>/dev/null; then
    # FreeBSD: usbconfig list or camcontrol devlist; Linux: lsusb
    USB_OUT="$(gssh "if command -v usbconfig >/dev/null 2>&1; then usbconfig list; elif command -v lsusb >/dev/null 2>&1; then lsusb; else exit 127; fi" 2>/dev/null || true)"
    if [ -n "${USB_OUT}" ]; then
        DEV_COUNT="$(printf '%s' "${USB_OUT}" | grep -c 'ugen\|Bus\|Device' 2>/dev/null || echo 0)"
        pass "usb: USB enumeration inside guest: ${DEV_COUNT} device(s) visible"
    else
        # Some minimal guest images may not have USB enumeration tools
        skip "usb: usbconfig/lsusb not available in guest image"
    fi
else
    skip "usb: USB enumeration check via SSH (SSH not available)"
fi

# ── Summary ────────────────────────────────────────────────────────────────────

printf "\n  device_contracts: ${GREEN}%d passed${RESET}, ${RED}%d failed${RESET}, ${YELLOW}%d skipped${RESET}\n\n" \
    "${PASS}" "${FAIL}" "${SKIP}"
[ "${FAIL}" -eq 0 ] || exit 1
exit 0
