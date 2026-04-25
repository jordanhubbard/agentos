#!/usr/bin/env bash
# agentOS E2E — guest_contract.h lifecycle tests
#
# Exercises the full MSG_GUEST_* lifecycle:
#   MSG_GUEST_CREATE → MSG_GUEST_BIND_DEVICE → MSG_GUEST_SET_MEMORY →
#   MSG_GUEST_BOOT → MSG_GUEST_SUSPEND → MSG_GUEST_RESUME → MSG_GUEST_DESTROY
#
# Slot 0 is auto-started by the FreeBSD manifest; this script verifies its
# state and exercises slot 1 (on-demand) via the CC bridge.
#
# Exit codes:
#   0 — PASS
#   1 — FAIL
#   2 — SKIP (bridge not available)

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
BRIDGE_AVAIL="${BRIDGE_AVAILABLE:-0}"
VMM_TYPE="${E2E_GUEST_VMM_TYPE:-freebsd}"

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
    printf '%s' "$1" | grep -o '"ok":[^,}]*' | head -1 | grep -q '"ok":true'
}

# ── Skip if bridge not available ───────────────────────────────────────────────

if [ "${BRIDGE_AVAIL}" = "0" ]; then
    skip "guest_contract: CC bridge not available — skipping lifecycle tests"
    printf "  (filed as known gap: bridge must expose /api/agentos/cc/guest/* endpoints)\n"
    exit 2
fi

# ── Test 1: CC_LIST_GUESTS — slot 0 should be running ─────────────────────────

RESP="$(cc_get "guests" 2>/dev/null)"
if ok_field "${RESP}"; then
    COUNT="$(printf '%s' "${RESP}" | grep -o '"count":[0-9]*' | grep -o '[0-9]*')"
    if [ "${COUNT:-0}" -ge 1 ]; then
        pass "MSG_CC_LIST_GUESTS: ${COUNT} guest(s) running (slot 0 auto-started)"
    else
        fail "MSG_CC_LIST_GUESTS: returned ok but count=${COUNT:-?} (expected ≥1)"
    fi
else
    fail "MSG_CC_LIST_GUESTS: request failed or ok:false (response: ${RESP:-<empty>})"
fi

# ── Test 2: CC_GUEST_STATUS — slot 0 state == RUNNING ─────────────────────────

RESP="$(cc_post "guest/status" '{"guest_id":0}')"
if ok_field "${RESP}"; then
    STATE="$(printf '%s' "${RESP}" | grep -o '"state":"[^"]*"' | head -1)"
    if printf '%s' "${STATE}" | grep -qi '"state":"running"'; then
        pass "MSG_CC_GUEST_STATUS: slot 0 state=RUNNING"
    else
        fail "MSG_CC_GUEST_STATUS: slot 0 state unexpected: ${STATE:-?}"
    fi
else
    fail "MSG_CC_GUEST_STATUS: request failed (response: ${RESP:-<empty>})"
fi

# ── Test 3: MSG_GUEST_CREATE — create slot 1 on demand ────────────────────────

RESP="$(cc_post "guest/create" "{\"slot\":1,\"vmm_type\":\"${VMM_TYPE}\"}")"
if ok_field "${RESP}"; then
    GUEST_ID="$(printf '%s' "${RESP}" | grep -o '"guest_id":[0-9]*' | grep -o '[0-9]*')"
    pass "MSG_GUEST_CREATE: slot 1 created (guest_id=${GUEST_ID:-?})"
    SLOT1_ID="${GUEST_ID:-1}"
else
    fail "MSG_GUEST_CREATE: failed (response: ${RESP:-<empty>})"
    SLOT1_ID=""
fi

# ── Test 4: MSG_GUEST_BIND_DEVICE — bind serial, net, block ───────────────────

if [ -n "${SLOT1_ID:-}" ]; then
    for DEV in serial net block; do
        RESP="$(cc_post "guest/bind-device" \
            "{\"guest_id\":${SLOT1_ID},\"dev_type\":\"${DEV}\"}")"
        if ok_field "${RESP}"; then
            CAP_TOKEN="$(printf '%s' "${RESP}" | grep -o '"cap_token":[0-9]*' | grep -o '[0-9]*')"
            pass "MSG_GUEST_BIND_DEVICE(${DEV}): cap_token=${CAP_TOKEN:-?}"
        else
            fail "MSG_GUEST_BIND_DEVICE(${DEV}): failed (${RESP:-<empty>})"
        fi
    done

    # ── Test 5: MSG_GUEST_SET_MEMORY ──────────────────────────────────────────

    RESP="$(cc_post "guest/set-memory" \
        "{\"guest_id\":${SLOT1_ID},\"size_mb\":512}")"
    if ok_field "${RESP}"; then
        pass "MSG_GUEST_SET_MEMORY: 512MB allocated for slot 1"
    else
        fail "MSG_GUEST_SET_MEMORY: failed (${RESP:-<empty>})"
    fi

    # ── Test 6: MSG_GUEST_BOOT ────────────────────────────────────────────────

    RESP="$(cc_post "guest/boot" "{\"guest_id\":${SLOT1_ID}}")"
    if ok_field "${RESP}"; then
        pass "MSG_GUEST_BOOT: slot 1 boot command sent"
    else
        fail "MSG_GUEST_BOOT: failed (${RESP:-<empty>})"
    fi

    # Wait briefly for boot to progress
    sleep 5

    # ── Test 7: MSG_CC_GUEST_STATUS — slot 1 transitioning or running ─────────

    RESP="$(cc_post "guest/status" "{\"guest_id\":${SLOT1_ID}}")"
    if ok_field "${RESP}"; then
        STATE="$(printf '%s' "${RESP}" | grep -o '"state":"[^"]*"' | head -1)"
        case "${STATE}" in
            *running*|*booting*)
                pass "MSG_CC_GUEST_STATUS: slot 1 state=${STATE} (boot progressing)"
                ;;
            *)
                fail "MSG_CC_GUEST_STATUS: slot 1 unexpected state ${STATE:-?}"
                ;;
        esac
    else
        fail "MSG_CC_GUEST_STATUS slot 1: request failed"
    fi

    # ── Test 8: MSG_GUEST_SUSPEND / RESUME ────────────────────────────────────

    RESP="$(cc_post "guest/suspend" "{\"guest_id\":${SLOT1_ID}}")"
    if ok_field "${RESP}"; then
        pass "MSG_GUEST_SUSPEND: slot 1 suspended"
    else
        fail "MSG_GUEST_SUSPEND: failed (${RESP:-<empty>})"
    fi

    RESP="$(cc_post "guest/resume" "{\"guest_id\":${SLOT1_ID}}")"
    if ok_field "${RESP}"; then
        pass "MSG_GUEST_RESUME: slot 1 resumed"
    else
        fail "MSG_GUEST_RESUME: failed (${RESP:-<empty>})"
    fi

    # ── Test 9: MSG_GUEST_DESTROY ─────────────────────────────────────────────

    RESP="$(cc_post "guest/destroy" "{\"guest_id\":${SLOT1_ID}}")"
    if ok_field "${RESP}"; then
        pass "MSG_GUEST_DESTROY: slot 1 destroyed"
    else
        fail "MSG_GUEST_DESTROY: failed (${RESP:-<empty>})"
    fi

    # Verify slot 1 is gone
    RESP="$(cc_get "guests")"
    COUNT="$(printf '%s' "${RESP}" | grep -o '"count":[0-9]*' | grep -o '[0-9]*')"
    if [ "${COUNT:-1}" -le 1 ]; then
        pass "Post-destroy: guest count is ${COUNT:-?} (slot 1 removed)"
    else
        fail "Post-destroy: guest count is ${COUNT} (expected 1 — slot 0 only)"
    fi
else
    skip "guest slot 1 tests: skipped because MSG_GUEST_CREATE failed"
fi

# ── Summary ────────────────────────────────────────────────────────────────────

printf "\n  guest_contract: ${GREEN}%d passed${RESET}, ${RED}%d failed${RESET}, ${YELLOW}%d skipped${RESET}\n\n" \
    "${PASS}" "${FAIL}" "${SKIP}"
[ "${FAIL}" -eq 0 ] || exit 1
exit 0
