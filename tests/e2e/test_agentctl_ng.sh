#!/usr/bin/env bash
# agentctl-ng End-to-End Test
#
# Tests agentctl-ng --batch against a running agentOS QEMU environment.
#
# Exit codes:
#   0 — PASS
#   1 — FAIL
#   2 — SKIP (binary not built, QEMU not running, or socket not found)
#
# Environment variables (all optional):
#   CC_PD_SOCK        Path to cc_pd bridge socket (default: build/cc_pd.sock)
#   AGENTCTL_NG       Path to agentctl-ng binary (default: tools/agentctl-ng/agentctl-ng)
#   AGENTOS_SKIP_E2E  Skip this test unconditionally (set to any non-empty value)

set -euo pipefail

# ── Colour helpers ─────────────────────────────────────────────────────────────

if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'
    BOLD='\033[1m';   RESET='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BOLD=''; RESET=''
fi

pass() { printf "${GREEN}[PASS]${RESET} %s\n" "$*"; }
fail() { printf "${RED}[FAIL]${RESET} %s\n" "$*"; exit 1; }
skip() { printf "${YELLOW}[SKIP]${RESET} %s\n" "$*"; exit 2; }
info() { printf "${BOLD}[INFO]${RESET} %s\n" "$*"; }

# ── Configuration ──────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BINARY="${AGENTCTL_NG:-${REPO_ROOT}/tools/agentctl-ng/agentctl-ng}"
SOCK="${CC_PD_SOCK:-${REPO_ROOT}/build/cc_pd.sock}"

# ── Skip conditions ────────────────────────────────────────────────────────────

if [ -n "${AGENTOS_SKIP_E2E:-}" ]; then
    skip "AGENTOS_SKIP_E2E is set"
fi

if [ ! -f "${BINARY}" ]; then
    skip "agentctl-ng not built (expected: ${BINARY})"
fi

if [ ! -S "${SOCK}" ] && [ ! -e "${SOCK}" ]; then
    skip "cc_pd socket not found (expected: ${SOCK}) — start agentOS first"
fi

# ── Test 1: --batch list-guests ────────────────────────────────────────────────

info "Test 1: MSG_CC_LIST_GUESTS"
GUESTS_OUT="$("${BINARY}" --sock "${SOCK}" --batch list-guests 2>&1)"
EXIT_CODE=$?

if [ "${EXIT_CODE}" -ne 0 ]; then
    fail "list-guests exited ${EXIT_CODE}: ${GUESTS_OUT}"
fi

# Must print "guests: N" where N >= 1
if ! echo "${GUESTS_OUT}" | grep -qE '^guests: [1-9][0-9]*$'; then
    fail "list-guests output missing 'guests: N' (N>=1): ${GUESTS_OUT}"
fi

# Must include at least one handle= line
if ! echo "${GUESTS_OUT}" | grep -q 'handle='; then
    fail "list-guests missing guest entry lines: ${GUESTS_OUT}"
fi

pass "list-guests — $(echo "${GUESTS_OUT}" | head -1)"

# ── Test 2: --batch list-devices serial ───────────────────────────────────────

info "Test 2: MSG_CC_LIST_DEVICES(serial)"
DEVS_OUT="$("${BINARY}" --sock "${SOCK}" --batch list-devices serial 2>&1)"
EXIT_CODE=$?

if [ "${EXIT_CODE}" -ne 0 ]; then
    fail "list-devices serial exited ${EXIT_CODE}: ${DEVS_OUT}"
fi

if ! echo "${DEVS_OUT}" | grep -qE '^devices\(serial\)'; then
    fail "list-devices serial unexpected output: ${DEVS_OUT}"
fi

pass "list-devices serial — $(echo "${DEVS_OUT}" | head -1)"

# ── Test 3: --batch polecats ──────────────────────────────────────────────────

info "Test 3: MSG_CC_LIST_POLECATS"
PC_OUT="$("${BINARY}" --sock "${SOCK}" --batch polecats 2>&1)"
EXIT_CODE=$?

if [ "${EXIT_CODE}" -ne 0 ]; then
    fail "polecats exited ${EXIT_CODE}: ${PC_OUT}"
fi

if ! echo "${PC_OUT}" | grep -qE '^polecats: total=[0-9]+'; then
    fail "polecats unexpected output: ${PC_OUT}"
fi

pass "polecats — ${PC_OUT}"

# ── Test 4: --batch log-stream 0 0 ───────────────────────────────────────────

info "Test 4: MSG_CC_LOG_STREAM"
LOG_OUT="$("${BINARY}" --sock "${SOCK}" --batch log-stream 0 0 2>&1)"
EXIT_CODE=$?

if [ "${EXIT_CODE}" -ne 0 ]; then
    fail "log-stream exited ${EXIT_CODE}: ${LOG_OUT}"
fi

if ! echo "${LOG_OUT}" | grep -qE '^log: slot=0'; then
    fail "log-stream unexpected output: ${LOG_OUT}"
fi

pass "log-stream — ${LOG_OUT}"

# ── Summary ───────────────────────────────────────────────────────────────────

printf "\n${GREEN}${BOLD}All agentctl-ng e2e tests PASSED${RESET}\n\n"
exit 0
