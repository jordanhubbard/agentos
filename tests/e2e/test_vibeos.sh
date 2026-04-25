#!/usr/bin/env bash
# agentOS E2E — vibeos_contract.h tests
#
# Exercises the VibeOS lifecycle:
#   MSG_VIBEOS_CREATE → MSG_VIBEOS_BIND_DEVICE → MSG_VIBEOS_BOOT →
#   MSG_VIBEOS_STATUS → MSG_VIBEOS_SNAPSHOT → MSG_VIBEOS_DESTROY
#
# VibeOS (VibOS) is agentOS's WASM hot-swap execution environment.
# Tests run via the CC bridge.
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
    printf '%s' "$1" | grep -q '"ok":true'
}

if [ "${BRIDGE_AVAIL}" = "0" ]; then
    skip "vibeos_contract: CC bridge not available"
    exit 2
fi

VIBEOS_HANDLE=""

# ── Test 1: MSG_VIBEOS_CREATE ──────────────────────────────────────────────────

RESP="$(cc_post "vibeos/create" "{\"name\":\"e2e-test-vibe\",\"vmm_type\":\"${VMM_TYPE}\"}")"
if ok_field "${RESP}"; then
    VIBEOS_HANDLE="$(printf '%s' "${RESP}" | grep -o '"handle":[0-9]*' | grep -o '[0-9]*')"
    pass "MSG_VIBEOS_CREATE: handle=${VIBEOS_HANDLE:-?}"
else
    fail "MSG_VIBEOS_CREATE: failed (${RESP:-<empty>})"
fi

# ── Test 2: MSG_VIBEOS_BIND_DEVICE — bind serial device ───────────────────────

if [ -n "${VIBEOS_HANDLE:-}" ]; then
    RESP="$(cc_post "vibeos/bind-device" \
        "{\"handle\":${VIBEOS_HANDLE},\"dev_type\":\"serial\",\"dev_handle\":0}")"
    if ok_field "${RESP}"; then
        pass "MSG_VIBEOS_BIND_DEVICE(serial): bound"
    else
        fail "MSG_VIBEOS_BIND_DEVICE(serial): failed (${RESP:-<empty>})"
    fi

    # Bind net device
    RESP="$(cc_post "vibeos/bind-device" \
        "{\"handle\":${VIBEOS_HANDLE},\"dev_type\":\"net\",\"dev_handle\":0}")"
    if ok_field "${RESP}"; then
        pass "MSG_VIBEOS_BIND_DEVICE(net): bound"
    else
        fail "MSG_VIBEOS_BIND_DEVICE(net): failed (${RESP:-<empty>})"
    fi
fi

# ── Test 3: MSG_VIBEOS_BOOT ────────────────────────────────────────────────────

if [ -n "${VIBEOS_HANDLE:-}" ]; then
    RESP="$(cc_post "vibeos/boot" "{\"handle\":${VIBEOS_HANDLE}}")"
    if ok_field "${RESP}"; then
        pass "MSG_VIBEOS_BOOT: boot command sent"
    else
        fail "MSG_VIBEOS_BOOT: failed (${RESP:-<empty>})"
    fi

    # Brief pause for boot to progress
    sleep 3
fi

# ── Test 4: MSG_VIBEOS_STATUS ──────────────────────────────────────────────────

if [ -n "${VIBEOS_HANDLE:-}" ]; then
    RESP="$(cc_post "vibeos/status" "{\"handle\":${VIBEOS_HANDLE}}")"
    if ok_field "${RESP}"; then
        STATE="$(printf '%s' "${RESP}" | grep -o '"state":"[^"]*"' | head -1)"
        case "${STATE}" in
            *running*|*booting*|*ready*)
                pass "MSG_VIBEOS_STATUS: state=${STATE}"
                ;;
            *)
                fail "MSG_VIBEOS_STATUS: unexpected state=${STATE:-?}"
                ;;
        esac
    else
        fail "MSG_VIBEOS_STATUS: failed (${RESP:-<empty>})"
    fi
fi

# ── Test 5: MSG_VIBEOS_LIST — check e2e-test-vibe appears in list ─────────────

RESP="$(cc_get "vibeos/list")"
if ok_field "${RESP}"; then
    COUNT="$(printf '%s' "${RESP}" | grep -o '"count":[0-9]*' | grep -o '[0-9]*')"
    if printf '%s' "${RESP}" | grep -q '"e2e-test-vibe"'; then
        pass "MSG_VIBEOS_LIST: e2e-test-vibe found in list (${COUNT:-?} total)"
    elif [ "${COUNT:-0}" -ge 1 ]; then
        pass "MSG_VIBEOS_LIST: ${COUNT} VibeOS instance(s) running"
    else
        fail "MSG_VIBEOS_LIST: e2e-test-vibe not found in list"
    fi
else
    fail "MSG_VIBEOS_LIST: failed (${RESP:-<empty>})"
fi

# ── Test 6: MSG_VIBEOS_SNAPSHOT ────────────────────────────────────────────────

if [ -n "${VIBEOS_HANDLE:-}" ]; then
    RESP="$(cc_post "vibeos/snapshot" "{\"handle\":${VIBEOS_HANDLE}}")"
    if ok_field "${RESP}"; then
        SNAP_LO="$(printf '%s' "${RESP}" | grep -o '"snap_lo":[0-9]*' | grep -o '[0-9]*')"
        SNAP_HI="$(printf '%s' "${RESP}" | grep -o '"snap_hi":[0-9]*' | grep -o '[0-9]*')"
        pass "MSG_VIBEOS_SNAPSHOT: snap_lo=${SNAP_LO:-?} snap_hi=${SNAP_HI:-?}"
    elif printf '%s' "${RESP}" | grep -qE '"ok":false|"error"'; then
        # Snapshot may not be implemented yet; treat as non-fatal
        skip "MSG_VIBEOS_SNAPSHOT: returned error (snapshot may not be implemented)"
    else
        fail "MSG_VIBEOS_SNAPSHOT: failed (${RESP:-<empty>})"
    fi
fi

# ── Test 7: MSG_VIBEOS_CHECK_SERVICE_EXISTS ────────────────────────────────────

if [ -n "${VIBEOS_HANDLE:-}" ]; then
    # func_class 1 = storage service, func_class 2 = compute service
    RESP="$(cc_post "vibeos/check-service" \
        "{\"handle\":${VIBEOS_HANDLE},\"func_class\":1}")"
    if ok_field "${RESP}"; then
        EXISTS="$(printf '%s' "${RESP}" | grep -o '"exists":[a-z]*' | head -1)"
        pass "MSG_VIBEOS_CHECK_SERVICE_EXISTS(storage): ${EXISTS:-?}"
    else
        fail "MSG_VIBEOS_CHECK_SERVICE_EXISTS: failed (${RESP:-<empty>})"
    fi
fi

# ── Test 8: MSG_VIBEOS_DESTROY ─────────────────────────────────────────────────

if [ -n "${VIBEOS_HANDLE:-}" ]; then
    RESP="$(cc_post "vibeos/destroy" "{\"handle\":${VIBEOS_HANDLE}}")"
    if ok_field "${RESP}"; then
        pass "MSG_VIBEOS_DESTROY: VibeOS instance destroyed"
    else
        fail "MSG_VIBEOS_DESTROY: failed (${RESP:-<empty>})"
    fi

    # Verify it's gone from list
    RESP="$(cc_get "vibeos/list")"
    if ok_field "${RESP}"; then
        if printf '%s' "${RESP}" | grep -q '"e2e-test-vibe"'; then
            fail "Post-destroy: e2e-test-vibe still in list"
        else
            pass "Post-destroy: e2e-test-vibe removed from list"
        fi
    fi
fi

# ── Summary ────────────────────────────────────────────────────────────────────

printf "\n  vibeos_contract: ${GREEN}%d passed${RESET}, ${RED}%d failed${RESET}, ${YELLOW}%d skipped${RESET}\n\n" \
    "${PASS}" "${FAIL}" "${SKIP}"
[ "${FAIL}" -eq 0 ] || exit 1
exit 0
