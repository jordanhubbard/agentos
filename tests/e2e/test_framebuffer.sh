#!/usr/bin/env bash
# agentOS E2E — framebuffer_contract.h tests
#
# Tests MSG_FB_CREATE (NULL backend), MSG_FB_WRITE, MSG_FB_FLIP, MSG_FB_RESIZE,
# and MSG_FB_DESTROY via the CC bridge.
#
# The NULL backend discards all frame data; MSG_FB_FLIP increments frame_seq
# and returns ok.  No display hardware required.
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

cc_post() {
    curl -sf --max-time 5 \
        -X POST "${CC_BASE}/api/agentos/cc/$1" \
        -H "Content-Type: application/json" \
        -d "${2:-{}}" 2>/dev/null
}

ok_field() {
    printf '%s' "$1" | grep -q '"ok":true'
}

if [ "${BRIDGE_AVAIL}" = "0" ]; then
    skip "framebuffer_contract: CC bridge not available"
    exit 2
fi

# ── Test 1: MSG_FB_CREATE with NULL backend ────────────────────────────────────

RESP="$(cc_post "fb/create" '{"width":640,"height":480,"format":0,"backend":0}')"
# backend=0 is FB_BACKEND_NULL; format=0 is FB_FMT_XRGB8888
if ok_field "${RESP}"; then
    FB_HANDLE="$(printf '%s' "${RESP}" | grep -o '"handle":[0-9]*' | grep -o '[0-9]*')"
    if [ "${FB_HANDLE:-4294967295}" != "4294967295" ]; then
        pass "MSG_FB_CREATE (NULL backend, 640×480, XRGB8888): handle=${FB_HANDLE}"
    else
        fail "MSG_FB_CREATE: returned handle=0xFFFFFFFF (invalid)"
        FB_HANDLE=""
    fi
else
    fail "MSG_FB_CREATE: request failed (response: ${RESP:-<empty>})"
    FB_HANDLE=""
fi

# ── Test 2: MSG_FB_WRITE — write pixels (all zeros = black frame) ──────────────

if [ -n "${FB_HANDLE:-}" ]; then
    RESP="$(cc_post "fb/write" \
        "{\"handle\":${FB_HANDLE},\"x\":0,\"y\":0,\"width\":640,\"height\":480}")"
    if ok_field "${RESP}"; then
        pass "MSG_FB_WRITE: 640×480 pixel write accepted (NULL backend discards)"
    else
        fail "MSG_FB_WRITE: failed (${RESP:-<empty>})"
    fi
fi

# ── Test 3: MSG_FB_FLIP — commit frame; NULL backend increments frame_seq ─────

if [ -n "${FB_HANDLE:-}" ]; then
    RESP="$(cc_post "fb/flip" "{\"handle\":${FB_HANDLE}}")"
    if ok_field "${RESP}"; then
        FRAME_SEQ="$(printf '%s' "${RESP}" | grep -o '"frame_seq":[0-9]*' | grep -o '[0-9]*')"
        if [ "${FRAME_SEQ:-0}" -ge 1 ]; then
            pass "MSG_FB_FLIP: NULL backend flip ok (frame_seq=${FRAME_SEQ})"
        else
            fail "MSG_FB_FLIP: frame_seq not incremented (frame_seq=${FRAME_SEQ:-?})"
        fi
    else
        fail "MSG_FB_FLIP: failed (${RESP:-<empty>})"
    fi
fi

# ── Test 4: MSG_FB_FLIP again — frame_seq must increase monotonically ──────────

if [ -n "${FB_HANDLE:-}" ]; then
    FIRST_SEQ="${FRAME_SEQ:-0}"
    RESP="$(cc_post "fb/flip" "{\"handle\":${FB_HANDLE}}")"
    if ok_field "${RESP}"; then
        SECOND_SEQ="$(printf '%s' "${RESP}" | grep -o '"frame_seq":[0-9]*' | grep -o '[0-9]*')"
        if [ "${SECOND_SEQ:-0}" -gt "${FIRST_SEQ}" ]; then
            pass "MSG_FB_FLIP (2nd): frame_seq monotone (${FIRST_SEQ}→${SECOND_SEQ})"
        else
            fail "MSG_FB_FLIP (2nd): frame_seq not increasing (${FIRST_SEQ}→${SECOND_SEQ:-?})"
        fi
    else
        fail "MSG_FB_FLIP (2nd): failed"
    fi
fi

# ── Test 5: MSG_FB_RESIZE ──────────────────────────────────────────────────────

if [ -n "${FB_HANDLE:-}" ]; then
    RESP="$(cc_post "fb/resize" \
        "{\"handle\":${FB_HANDLE},\"width\":1024,\"height\":768}")"
    if ok_field "${RESP}"; then
        pass "MSG_FB_RESIZE: resized to 1024×768"
    else
        fail "MSG_FB_RESIZE: failed (${RESP:-<empty>})"
    fi
fi

# ── Test 6: MSG_FB_CREATE with HW_DIRECT backend — expect graceful error ──────

RESP="$(cc_post "fb/create" '{"width":640,"height":480,"format":0,"backend":1}')"
# backend=1 is FB_BACKEND_HW_DIRECT; this may fail if no GPU, but must not crash
if ok_field "${RESP}"; then
    HW_HANDLE="$(printf '%s' "${RESP}" | grep -o '"handle":[0-9]*' | grep -o '[0-9]*')"
    pass "MSG_FB_CREATE (HW_DIRECT backend): handle=${HW_HANDLE:-?} (GPU available)"
    # Clean up
    if [ "${HW_HANDLE:-4294967295}" != "4294967295" ]; then
        cc_post "fb/destroy" "{\"handle\":${HW_HANDLE}}" >/dev/null 2>&1
    fi
elif printf '%s' "${RESP}" | grep -qE '"ok":false|"error"'; then
    pass "MSG_FB_CREATE (HW_DIRECT backend): graceful error when no GPU (ok:false)"
else
    fail "MSG_FB_CREATE (HW_DIRECT): unexpected response: ${RESP:-<empty>}"
fi

# ── Test 7: MSG_FB_DESTROY ─────────────────────────────────────────────────────

if [ -n "${FB_HANDLE:-}" ]; then
    RESP="$(cc_post "fb/destroy" "{\"handle\":${FB_HANDLE}}")"
    if ok_field "${RESP}"; then
        pass "MSG_FB_DESTROY: framebuffer destroyed"
    else
        fail "MSG_FB_DESTROY: failed (${RESP:-<empty>})"
    fi

    # Verify handle is invalid after destroy
    RESP="$(cc_post "fb/flip" "{\"handle\":${FB_HANDLE}}")"
    if printf '%s' "${RESP}" | grep -qE '"ok":false|"error"'; then
        pass "Post-destroy: flip on destroyed handle returns error (handle invalidated)"
    else
        fail "Post-destroy: flip on destroyed handle did not return error"
    fi
fi

# ── Summary ────────────────────────────────────────────────────────────────────

printf "\n  framebuffer_contract: ${GREEN}%d passed${RESET}, ${RED}%d failed${RESET}, ${YELLOW}%d skipped${RESET}\n\n" \
    "${PASS}" "${FAIL}" "${SKIP}"
[ "${FAIL}" -eq 0 ] || exit 1
exit 0
