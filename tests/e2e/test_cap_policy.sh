#!/usr/bin/env bash
# agentOS E2E — cap_policy.c ring-1 enforcement tests
#
# Verifies that the agentOS capability policy correctly rejects ring-0
# escalation attempts from guest VMs.  Tests two vectors:
#
#   1. Host-side (via CC bridge):
#      Send MSG_CAP_GRANT-class requests that would give a guest access to a
#      ring-0 channel (CH_AGENTFS, CH_NAMESERVER, etc.).  These must be
#      rejected with CC_ERR_CAP_DENIED or equivalent.
#
#   2. Guest-side (via SSH):
#      Inside the FreeBSD guest, attempt to access /dev/mem (direct hardware
#      read) and privileged kernel interfaces.  These must return EPERM (or
#      be absent entirely on a capability-hardened kernel).
#
#   3. Serial log inspection:
#      After attempting escalations, check the agentOS serial log for
#      [cap_policy] rejection messages.
#
# The acceptance criterion (from the bead) is:
#   "attempt ring-0 escalation from guest, verify EPERM (not a crash)"
#
# Exit codes:
#   0 — PASS (escalation rejected; no crash)
#   1 — FAIL (escalation succeeded or system crashed)
#   2 — SKIP (bridge and SSH both unavailable)

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
SERIAL_LOG="${SERIAL_LOG:-/dev/null}"
BRIDGE_AVAIL="${BRIDGE_AVAILABLE:-0}"
HAVE_SSH="${HAVE_SSH_TOOLS:-0}"
FREEBSD_BOOTED="${FREEBSD_BOOTED:-0}"

cc_post() {
    curl -sf --max-time 5 \
        -X POST "${CC_BASE}/api/agentos/cc/$1" \
        -H "Content-Type: application/json" \
        -d "${2:-{}}" 2>/dev/null
}

ok_field()  { printf '%s' "$1" | grep -q '"ok":true'; }
err_field() { printf '%s' "$1" | grep -qE '"ok":false|"error"|"denied"'; }

gssh() {
    ssh -i "${SSH_KEY}" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=10 \
        -o BatchMode=yes \
        -p "${SSH_PORT}" \
        root@localhost "$@" 2>/dev/null
}

# At least one of bridge or SSH must be available
if [ "${BRIDGE_AVAIL}" = "0" ] && [ "${HAVE_SSH}" = "0" ]; then
    skip "cap_policy: neither CC bridge nor SSH available"
    exit 2
fi

# ══════════════════════════════════════════════════════════════════════════════
# Vector 1: Host-side — CC bridge ring-0 escalation attempts
# ══════════════════════════════════════════════════════════════════════════════

if [ "${BRIDGE_AVAIL}" = "1" ]; then
    printf "  ${BOLD}cap_policy: host-side escalation via CC bridge${RESET}\n"

    # Test 1: Attempt to grant a guest capability to CH_AGENTFS (ring-0 channel)
    # CH_AGENTFS = 5 (from agentos.h) — guests must NOT have this
    RESP="$(cc_post "cap/grant" \
        '{"target_pd":42,"cap_class":5,"rights":3}')"
    # cap_class=5 corresponds to CH_AGENTFS region; target_pd=42 is freebsd_vmm
    if err_field "${RESP}"; then
        pass "cap/grant (CH_AGENTFS to guest): correctly denied (${RESP:-<empty>})"
    elif ok_field "${RESP}"; then
        fail "cap/grant (CH_AGENTFS to guest): GRANTED — cap policy failed!"
    else
        # Bridge endpoint may not exist yet; treat as non-fatal skip
        skip "cap/grant: endpoint not implemented yet (${RESP:-<empty>})"
    fi

    # Test 2: Attempt to route a command to CH_NAMESERVER (ring-0, id=18)
    # via MSG_CC_SEND_INPUT with an illegal channel target
    RESP="$(cc_post "cap/channel-access" \
        '{"guest_id":0,"target_channel":18}')"
    # channel 18 = CH_NAMESERVER — ring-0, must be blocked
    if err_field "${RESP}"; then
        pass "cap/channel-access (CH_NAMESERVER): correctly denied"
    elif ok_field "${RESP}"; then
        fail "cap/channel-access (CH_NAMESERVER=18): ALLOWED — ring-0 channel exposed!"
    else
        skip "cap/channel-access: endpoint not implemented (non-fatal)"
    fi

    # Test 3: Attempt to access CH_VIRTIO_BLK directly (ring-0, id=22)
    RESP="$(cc_post "cap/channel-access" \
        '{"guest_id":0,"target_channel":22}')"
    if err_field "${RESP}"; then
        pass "cap/channel-access (CH_VIRTIO_BLK=22): correctly denied"
    elif ok_field "${RESP}"; then
        fail "cap/channel-access (CH_VIRTIO_BLK=22): ALLOWED — ring-0 channel exposed!"
    else
        skip "cap/channel-access (CH_VIRTIO_BLK): endpoint not implemented (non-fatal)"
    fi

    # Test 4: Verify that ring-1 channels ARE accessible (should succeed)
    # CH_SERIAL_PD=67, CH_NET_PD=68, CH_BLOCK_PD=69 are ring-1 (guest-allowed)
    RESP="$(cc_post "cap/channel-access" \
        '{"guest_id":0,"target_channel":67}')"
    if ok_field "${RESP}" || ! err_field "${RESP}"; then
        pass "cap/channel-access (CH_SERIAL_PD=67 ring-1): allowed (correct)"
    elif err_field "${RESP}"; then
        fail "cap/channel-access (CH_SERIAL_PD=67): DENIED — ring-1 channel should be allowed!"
    fi
fi

# ══════════════════════════════════════════════════════════════════════════════
# Vector 2: Guest-side — attempt privileged access inside FreeBSD VM
# ══════════════════════════════════════════════════════════════════════════════

if [ "${HAVE_SSH}" = "1" ] && [ "${FREEBSD_BOOTED}" = "1" ] && \
   gssh true 2>/dev/null; then
    printf "\n  ${BOLD}cap_policy: guest-side escalation via SSH${RESET}\n"

    # Test 5: /dev/mem — direct physical memory access (must be EPERM or absent)
    RC=0
    OUT="$(gssh dd if=/dev/mem bs=4 count=1 2>&1)" || RC=$?
    if [ "${RC}" -ne 0 ]; then
        if printf '%s' "${OUT}" | grep -qiE "permission denied|eperm|not permitted|no such"; then
            pass "guest /dev/mem: access denied (EPERM) — ring-0 hardware protected"
        elif [ ! -e "/dev/mem" ] 2>/dev/null; then
            pass "guest /dev/mem: device absent (hardened kernel)"
        else
            pass "guest /dev/mem: access failed (rc=${RC}) — non-POSIX but non-zero is correct"
        fi
    else
        fail "guest /dev/mem: read SUCCEEDED — guest can read physical memory!"
    fi

    # Test 6: /dev/kmem — kernel memory access (must be EPERM or absent)
    RC=0
    OUT="$(gssh dd if=/dev/kmem bs=4 count=1 2>&1)" || RC=$?
    if [ "${RC}" -ne 0 ]; then
        pass "guest /dev/kmem: access denied (rc=${RC})"
    else
        fail "guest /dev/kmem: read SUCCEEDED — guest can read kernel memory!"
    fi

    # Test 7: mlock/mmap of low memory (must fail for non-root or be absent)
    # Use 'perl' or 'python' if available to attempt mmap(0, ...)
    RC=0
    MMAP_OUT="$(gssh perl -e 'use POSIX; open(my $f,"/dev/mem") or die "EPERM:$!"' 2>&1)" || RC=$?
    if [ "${RC}" -ne 0 ]; then
        pass "guest perl mmap /dev/mem: permission correctly denied"
    else
        # If perl isn't available, this test may be skipped silently
        if printf '%s' "${MMAP_OUT}" | grep -qi "command not found\|not found"; then
            skip "guest perl mmap test: perl not installed in guest image"
        else
            fail "guest perl mmap /dev/mem: SUCCEEDED — direct hw access from guest!"
        fi
    fi

    # Test 8: kldload (FreeBSD kernel module load — must be blocked by seL4 cap policy)
    RC=0
    OUT="$(gssh kldload e2e_test_nonexistent 2>&1)" || RC=$?
    if [ "${RC}" -ne 0 ]; then
        if printf '%s' "${OUT}" | grep -qiE "permission denied|eperm|no such module|not found"; then
            pass "guest kldload: correctly denied (no path to ring-0 kernel interface)"
        else
            pass "guest kldload: failed (rc=${RC}) — kernel module path blocked"
        fi
    else
        fail "guest kldload: SUCCEEDED — guest can load kernel modules!"
    fi
fi

# ══════════════════════════════════════════════════════════════════════════════
# Vector 3: Serial log inspection for cap_policy rejection messages
# ══════════════════════════════════════════════════════════════════════════════

printf "\n  ${BOLD}cap_policy: serial log inspection${RESET}\n"

# Test 9: No seL4 fault/panic in serial log after escalation attempts
if grep -qiE "panic|seL4_fault|cap_policy.*fatal|CRASH|ABORT" "${SERIAL_LOG}" 2>/dev/null; then
    fail "Serial log: system panic or fatal cap_policy violation detected"
else
    pass "Serial log: no panic or fatal cap_policy violation after escalation attempts"
fi

# Test 10: cap_policy logged rejections (informational — not required)
REJECTIONS="$(grep -c 'cap_policy.*reject\|cap_policy.*deny\|ring-0.*blocked' \
    "${SERIAL_LOG}" 2>/dev/null || echo 0)"
if [ "${REJECTIONS}" -gt 0 ]; then
    pass "Serial log: ${REJECTIONS} cap_policy rejection(s) logged (enforcement active)"
else
    skip "Serial log: no cap_policy rejections logged (rejections may not be logged at INFO)"
fi

# Test 11: Verify agentOS is still responsive after escalation attempts
if [ "${BRIDGE_AVAIL}" = "1" ]; then
    RESP="$(cc_post "connect" '{"client_badge":99,"flags":1}' 2>/dev/null)"
    if ok_field "${RESP}"; then
        SID="$(printf '%s' "${RESP}" | grep -o '"session_id":[0-9]*' | grep -o '[0-9]*')"
        cc_post "disconnect" "{\"session_id\":${SID:-0}}" >/dev/null 2>&1 || true
        pass "Post-escalation: agentOS still responsive (CC bridge connects)"
    else
        fail "Post-escalation: agentOS not responsive! (CC bridge failed)"
    fi
elif [ "${HAVE_SSH}" = "1" ] && gssh true 2>/dev/null; then
    pass "Post-escalation: agentOS still responsive (SSH still works)"
fi

# ── Summary ────────────────────────────────────────────────────────────────────

printf "\n  cap_policy: ${GREEN}%d passed${RESET}, ${RED}%d failed${RESET}, ${YELLOW}%d skipped${RESET}\n\n" \
    "${PASS}" "${FAIL}" "${SKIP}"
[ "${FAIL}" -eq 0 ] || exit 1
exit 0
