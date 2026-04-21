#!/usr/bin/env bash
# agentOS E2E — cc_contract.h tests
#
# Tests the Command-and-Control PD relay API:
#   MSG_CC_CONNECT / MSG_CC_DISCONNECT (session management)
#   MSG_CC_LIST_GUESTS                 (MSG_CC_LIST_GUESTS returns running guest)
#   MSG_CC_GUEST_STATUS                (accurate state for running slot 0)
#   MSG_CC_LIST_DEVICES                (device enumeration per type)
#   MSG_CC_LIST_POLECATS               (agent pool status)
#   MSG_CC_LIST                        (active session list)
#   MSG_CC_LOG_STREAM                  (log drain access)
#
# cc_pd contains ZERO policy — it is a pure relay.  Every test verifies that
# the relay routes to the correct downstream PD and returns a coherent response.
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
FREEBSD_BOOTED="${FREEBSD_BOOTED:-0}"

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
    skip "cc_contract: CC bridge not available"
    exit 2
fi

# ── Test 1: MSG_CC_CONNECT — establish session ─────────────────────────────────

RESP="$(cc_post "connect" '{"client_badge":1,"flags":1}')"
# flags=1 is CC_CONNECT_FLAG_JSON
if ok_field "${RESP}"; then
    SESSION_ID="$(printf '%s' "${RESP}" | grep -o '"session_id":[0-9]*' | grep -o '[0-9]*')"
    pass "MSG_CC_CONNECT: session_id=${SESSION_ID:-?}"
else
    fail "MSG_CC_CONNECT: failed (${RESP:-<empty>})"
    SESSION_ID=""
fi

# ── Test 2: MSG_CC_STATUS — session is active ──────────────────────────────────

if [ -n "${SESSION_ID:-}" ]; then
    RESP="$(cc_post "session/status" "{\"session_id\":${SESSION_ID}}")"
    if ok_field "${RESP}"; then
        STATE="$(printf '%s' "${RESP}" | grep -o '"state":[0-9]*' | grep -o '[0-9]*')"
        # CC_SESSION_STATE_IDLE = 1, CC_SESSION_STATE_CONNECTED = 0
        case "${STATE:-}" in
            0|1) pass "MSG_CC_STATUS: session active (state=${STATE})" ;;
            *)   fail "MSG_CC_STATUS: unexpected state=${STATE:-?}" ;;
        esac
    else
        fail "MSG_CC_STATUS: failed (${RESP:-<empty>})"
    fi
fi

# ── Test 3: MSG_CC_LIST — active sessions ─────────────────────────────────────

RESP="$(cc_get "sessions")"
if ok_field "${RESP}"; then
    COUNT="$(printf '%s' "${RESP}" | grep -o '"count":[0-9]*' | grep -o '[0-9]*')"
    if [ "${COUNT:-0}" -ge 1 ]; then
        pass "MSG_CC_LIST: ${COUNT} active session(s)"
    else
        fail "MSG_CC_LIST: no sessions reported (count=${COUNT:-?})"
    fi
else
    fail "MSG_CC_LIST: failed (${RESP:-<empty>})"
fi

# ── Test 4: MSG_CC_LIST_GUESTS — returns running guest ────────────────────────

RESP="$(cc_get "guests")"
if ok_field "${RESP}"; then
    COUNT="$(printf '%s' "${RESP}" | grep -o '"count":[0-9]*' | grep -o '[0-9]*')"
    if [ "${FREEBSD_BOOTED}" = "1" ]; then
        if [ "${COUNT:-0}" -ge 1 ]; then
            pass "MSG_CC_LIST_GUESTS: ${COUNT} guest(s) running (FreeBSD slot 0 confirmed)"
        else
            fail "MSG_CC_LIST_GUESTS: count=${COUNT:-?} but FreeBSD should be running"
        fi
    else
        pass "MSG_CC_LIST_GUESTS: returned ok, count=${COUNT:-?}"
    fi
else
    fail "MSG_CC_LIST_GUESTS: failed (${RESP:-<empty>})"
fi

# ── Test 5: MSG_CC_GUEST_STATUS — accurate state ──────────────────────────────

if [ "${FREEBSD_BOOTED}" = "1" ]; then
    RESP="$(cc_post "guest/status" '{"guest_id":0}')"
    if ok_field "${RESP}"; then
        STATE="$(printf '%s' "${RESP}" | grep -o '"state":"[^"]*"' | head -1)"
        IP="$(printf '%s' "${RESP}" | grep -oE '"ip":"[0-9.]+"' | head -1)"
        UPTIME="$(printf '%s' "${RESP}" | grep -o '"uptime_s":[0-9]*' | grep -o '[0-9]*')"
        if printf '%s' "${STATE}" | grep -qi "running"; then
            pass "MSG_CC_GUEST_STATUS(slot 0): state=running, ${IP:-ip=unknown}, uptime=${UPTIME:-?}s"
        else
            fail "MSG_CC_GUEST_STATUS(slot 0): state=${STATE:-?} (expected running)"
        fi
    else
        fail "MSG_CC_GUEST_STATUS(slot 0): failed (${RESP:-<empty>})"
    fi
else
    skip "MSG_CC_GUEST_STATUS: FreeBSD not booted"
fi

# ── Test 6: MSG_CC_LIST_DEVICES — each device type ────────────────────────────

for DEV_TYPE in serial net block usb framebuffer; do
    RESP="$(cc_post "devices" "{\"dev_type\":\"${DEV_TYPE}\"}")"
    if ok_field "${RESP}"; then
        COUNT="$(printf '%s' "${RESP}" | grep -o '"count":[0-9]*' | grep -o '[0-9]*')"
        pass "MSG_CC_LIST_DEVICES(${DEV_TYPE}): ${COUNT:-?} device(s)"
    else
        fail "MSG_CC_LIST_DEVICES(${DEV_TYPE}): failed (${RESP:-<empty>})"
    fi
done

# ── Test 7: MSG_CC_LIST_POLECATS — agent pool status ─────────────────────────

RESP="$(cc_get "polecats")"
if ok_field "${RESP}"; then
    TOTAL="$(printf '%s' "${RESP}" | grep -o '"total":[0-9]*' | grep -o '[0-9]*')"
    BUSY="$(printf '%s'  "${RESP}" | grep -o '"busy":[0-9]*'  | grep -o '[0-9]*')"
    IDLE="$(printf '%s'  "${RESP}" | grep -o '"idle":[0-9]*'  | grep -o '[0-9]*')"
    pass "MSG_CC_LIST_POLECATS: total=${TOTAL:-?} busy=${BUSY:-?} idle=${IDLE:-?}"
else
    fail "MSG_CC_LIST_POLECATS: failed (${RESP:-<empty>})"
fi

# ── Test 8: MSG_CC_LOG_STREAM — drain logs from a PD ─────────────────────────

RESP="$(cc_post "log/stream" '{"slot":0,"pd_id":0}')"
if ok_field "${RESP}"; then
    BYTES="$(printf '%s' "${RESP}" | grep -o '"bytes_drained":[0-9]*' | grep -o '[0-9]*')"
    pass "MSG_CC_LOG_STREAM: ${BYTES:-?} bytes drained from log drain"
else
    fail "MSG_CC_LOG_STREAM: failed (${RESP:-<empty>})"
fi

# ── Test 9: MSG_CC_DISCONNECT — clean session teardown ────────────────────────

if [ -n "${SESSION_ID:-}" ]; then
    RESP="$(cc_post "disconnect" "{\"session_id\":${SESSION_ID}}")"
    if ok_field "${RESP}"; then
        pass "MSG_CC_DISCONNECT: session ${SESSION_ID} closed"
    else
        fail "MSG_CC_DISCONNECT: failed (${RESP:-<empty>})"
    fi

    # Verify session is gone
    RESP="$(cc_post "session/status" "{\"session_id\":${SESSION_ID}}")"
    if printf '%s' "${RESP}" | grep -qE '"state":3|"error"'; then
        # CC_SESSION_STATE_EXPIRED=3 or error
        pass "Post-disconnect: session expired/gone"
    else
        fail "Post-disconnect: session still active after disconnect"
    fi
fi

# ── Test 10: Multiple concurrent sessions (up to CC_MAX_SESSIONS=8) ──────────

SESSIONS=""
ALL_OK=1
for i in 1 2 3; do
    RESP="$(cc_post "connect" "{\"client_badge\":$((10 + i)),\"flags\":1}")"
    if ok_field "${RESP}"; then
        SID="$(printf '%s' "${RESP}" | grep -o '"session_id":[0-9]*' | grep -o '[0-9]*')"
        SESSIONS="${SESSIONS} ${SID}"
    else
        ALL_OK=0
        break
    fi
done

if [ "${ALL_OK}" -eq 1 ]; then
    pass "MSG_CC_CONNECT (concurrent): 3 additional sessions established"
    # Clean up
    for SID in ${SESSIONS}; do
        cc_post "disconnect" "{\"session_id\":${SID}}" >/dev/null 2>&1 || true
    done
else
    fail "MSG_CC_CONNECT (concurrent): failed to establish multiple sessions"
fi

# ── Summary ────────────────────────────────────────────────────────────────────

printf "\n  cc_contract: ${GREEN}%d passed${RESET}, ${RED}%d failed${RESET}, ${YELLOW}%d skipped${RESET}\n\n" \
    "${PASS}" "${FAIL}" "${SKIP}"
[ "${FAIL}" -eq 0 ] || exit 1
exit 0
