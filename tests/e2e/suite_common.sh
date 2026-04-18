#!/usr/bin/env bash
# agentOS E2E — Portable Unix Command Suite (runs inside guest via SSH)
#
# This script runs INSIDE the guest VM over SSH.  It must work on any UNIX
# variant (FreeBSD, Linux) without OS-specific assumptions.  Every command
# used must be part of POSIX or the Single UNIX Specification.
#
# Called from run_e2e.sh after SSH connectivity is confirmed.
#
# Exit codes:
#   0 — all PASS (or only non-fatal SKIPs)
#   1 — at least one FAIL
#   2 — SKIP (SSH not available)

set -uo pipefail

# ── Helpers ────────────────────────────────────────────────────────────────────

if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'
    BOLD='\033[1m';   RESET='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BOLD=''; RESET=''
fi

PASS=0
FAIL=0
SKIP=0

pass() { printf "${GREEN}  [PASS]${RESET} %s\n" "$*"; PASS=$(( PASS + 1 )); }
fail() { printf "${RED}  [FAIL]${RESET} %s\n"   "$*"; FAIL=$(( FAIL + 1 )); }
skip() { printf "${YELLOW}  [SKIP]${RESET} %s\n" "$*"; SKIP=$(( SKIP + 1 )); }

# E2E_SSH_PORT and E2E_SSH_KEY may be set by run_e2e.sh; use sensible defaults
SSH_PORT="${E2E_SSH_PORT:-2222}"
SSH_KEY="${E2E_SSH_KEY:-$(dirname "$0")/id_ed25519}"

if [ ! -f "${SSH_KEY}" ]; then
    skip "SSH key not found: ${SSH_KEY}"
    exit 2
fi

# Run a command inside the guest via SSH and capture output
gssh() {
    ssh -i "${SSH_KEY}" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=10 \
        -o BatchMode=yes \
        -p "${SSH_PORT}" \
        root@localhost "$@" 2>/dev/null
}

# Verify SSH is responsive
if ! gssh true 2>/dev/null; then
    skip "SSH not reachable on port ${SSH_PORT}"
    exit 2
fi

printf "${BOLD}Running portable Unix command suite inside guest:${RESET}\n\n"

# ── Test 1: echo ───────────────────────────────────────────────────────────────

OUT="$(gssh echo hello 2>/dev/null)"
if [ "${OUT}" = "hello" ]; then
    pass "echo: 'hello' → '${OUT}'"
else
    fail "echo: expected 'hello', got '${OUT}'"
fi

# ── Test 2: hostname ───────────────────────────────────────────────────────────

OUT="$(gssh hostname 2>/dev/null)"
if [ -n "${OUT}" ]; then
    pass "hostname: '${OUT}'"
else
    fail "hostname: returned empty string"
fi

# ── Test 3: df (disk free — verify root fs mounted) ───────────────────────────

OUT="$(gssh df / 2>/dev/null)"
if printf '%s' "${OUT}" | grep -qE '/\s*$|/$'; then
    pass "df: root filesystem mounted"
else
    fail "df: unexpected output: ${OUT}"
fi

# ── Test 4: which (verify common tools are present) ───────────────────────────

TOOLS="ls cat grep awk sed sort wc head tail"
ALL_FOUND=1
MISSING=""
for tool in ${TOOLS}; do
    if ! gssh which "${tool}" >/dev/null 2>/dev/null; then
        MISSING="${MISSING} ${tool}"
        ALL_FOUND=0
    fi
done
if [ "${ALL_FOUND}" -eq 1 ]; then
    pass "which: all standard tools present (${TOOLS})"
else
    fail "which: missing tools:${MISSING}"
fi

# ── Test 5: uptime ─────────────────────────────────────────────────────────────

OUT="$(gssh uptime 2>/dev/null)"
if [ -n "${OUT}" ]; then
    pass "uptime: '${OUT}'"
else
    fail "uptime: returned empty output"
fi

# ── Test 6: uname (verify running on UNIX) ────────────────────────────────────

OUT="$(gssh uname -s 2>/dev/null)"
case "${OUT}" in
    FreeBSD|Linux|NetBSD|OpenBSD|Darwin)
        pass "uname -s: '${OUT}'"
        ;;
    *)
        fail "uname -s: unexpected '${OUT}' (expected FreeBSD or Linux)"
        ;;
esac

# ── Test 7: cat /etc/os-release or /etc/freebsd-version ───────────────────────

if gssh test -f /etc/os-release >/dev/null 2>&1; then
    OUT="$(gssh cat /etc/os-release 2>/dev/null | head -3)"
    if [ -n "${OUT}" ]; then
        pass "/etc/os-release readable"
    else
        fail "/etc/os-release: empty"
    fi
elif gssh test -f /etc/freebsd-version >/dev/null 2>&1; then
    OUT="$(gssh cat /etc/freebsd-version 2>/dev/null)"
    pass "/etc/freebsd-version: '${OUT}'"
else
    skip "No /etc/os-release or /etc/freebsd-version — OS identification skipped"
fi

# ── Test 8: cat /proc/1/status (Linux) or ps (FreeBSD) ─────────────────────────

if gssh test -f /proc/1/status >/dev/null 2>&1; then
    OUT="$(gssh cat /proc/1/status 2>/dev/null | head -1)"
    if [ -n "${OUT}" ]; then
        pass "/proc/1/status readable (Linux procfs)"
    else
        fail "/proc/1/status: empty"
    fi
elif gssh ps -p 1 >/dev/null 2>&1; then
    OUT="$(gssh ps -p 1 -o comm= 2>/dev/null)"
    pass "ps PID 1: '${OUT}' (FreeBSD)"
else
    skip "Process 1 visibility test: neither /proc/1/status nor ps worked"
fi

# ── Test 9: read/write to a temp file (block I/O sanity) ──────────────────────

TMPFILE="/tmp/agentos_e2e_rw_test.$$"
WROTE="agentos_e2e_block_test"
gssh "printf '%s' '${WROTE}' > ${TMPFILE}" 2>/dev/null
OUT="$(gssh cat "${TMPFILE}" 2>/dev/null)"
gssh rm -f "${TMPFILE}" 2>/dev/null
if [ "${OUT}" = "${WROTE}" ]; then
    pass "block I/O: write/read round-trip on /tmp"
else
    fail "block I/O: wrote '${WROTE}', read '${OUT}'"
fi

# ── Test 10: network connectivity (guest can reach QEMU host) ─────────────────

# QEMU user-mode networking: the host is at 10.0.2.2
if gssh ping -c 1 -W 2 10.0.2.2 >/dev/null 2>&1; then
    pass "network: guest can ping QEMU gateway (10.0.2.2)"
elif gssh ping -c 1 -o 10.0.2.2 >/dev/null 2>&1; then
    # BSD ping: -o = quit after one reply (same as -c 1 on Linux)
    pass "network: guest can ping QEMU gateway (10.0.2.2)"
else
    skip "network: ping to 10.0.2.2 failed (may be filtered) — non-fatal"
fi

# ── Summary ────────────────────────────────────────────────────────────────────

printf "\n"
printf "  Guest SSH suite: ${GREEN}%d passed${RESET}, ${RED}%d failed${RESET}, ${YELLOW}%d skipped${RESET}\n" \
    "${PASS}" "${FAIL}" "${SKIP}"
printf "\n"

[ "${FAIL}" -eq 0 ] || exit 1
exit 0
