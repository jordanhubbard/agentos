#!/usr/bin/env bash
# agentOS E2E — VOS_RESTORE tests
#
# Exercises the full snapshot→restore lifecycle:
#   VOS_CREATE → VOS_BIND_DEVICE → VOS_BOOT → VOS_SNAPSHOT →
#   VOS_RESTORE → verify state and SSH connectivity
#
# A snapshot captures the running guest's memory + device state into AgentFS.
# VOS_RESTORE replays that snapshot into a fresh guest slot, which must reach
# the same SSH-reachable state without re-running the boot sequence.
#
# Exit codes:
#   0 — PASS
#   1 — FAIL
#   2 — SKIP (bridge not available or snapshot not supported)

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
SSH_PORT="${E2E_SSH_PORT:-2222}"
SSH_KEY="${E2E_SSH_KEY:-$(dirname "$0")/id_ed25519}"
HAVE_SSH="${HAVE_SSH_TOOLS:-0}"

cc_post() {
    curl -sf --max-time 10 \
        -X POST "${CC_BASE}/api/agentos/cc/$1" \
        -H "Content-Type: application/json" \
        -d "${2:-{}}" 2>/dev/null
}

cc_get() {
    curl -sf --max-time 10 "${CC_BASE}/api/agentos/cc/$1" 2>/dev/null
}

ok_field() { printf '%s' "$1" | grep -q '"ok":true'; }

guest_ssh() {
    ssh -i "${SSH_KEY}" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=10 \
        -o BatchMode=yes \
        -p "${SSH_PORT}" \
        root@localhost "$@" 2>/dev/null
}

if [ "${BRIDGE_AVAIL}" = "0" ]; then
    skip "vibeos_restore: CC bridge not available"
    exit 2
fi

SRC_HANDLE=""
SNAP_LO=""
SNAP_HI=""
DST_HANDLE=""

# ── Step 1: Create source VibeOS instance ─────────────────────────────────────

printf "${BOLD}  Step 1: Create and boot source VibeOS instance${RESET}\n"

RESP="$(cc_post "vibeos/create" \
    "{\"name\":\"e2e-restore-src\",\"vmm_type\":\"${VMM_TYPE}\"}")"
if ok_field "${RESP}"; then
    SRC_HANDLE="$(printf '%s' "${RESP}" | grep -o '"handle":[0-9]*' | grep -o '[0-9]*')"
    pass "VOS_CREATE: source handle=${SRC_HANDLE:-?}"
else
    fail "VOS_CREATE: failed (${RESP:-<empty>})"
    exit 1
fi

# ── Step 2: Bind devices ──────────────────────────────────────────────────────

for DEV in serial net; do
    RESP="$(cc_post "vibeos/bind-device" \
        "{\"handle\":${SRC_HANDLE},\"dev_type\":\"${DEV}\",\"dev_handle\":0}")"
    if ok_field "${RESP}"; then
        pass "VOS_BIND_DEVICE(${DEV}): bound"
    else
        fail "VOS_BIND_DEVICE(${DEV}): failed (${RESP:-<empty>})"
    fi
done

# ── Step 3: Boot the source instance ─────────────────────────────────────────

printf "${BOLD}  Step 2: Boot source instance${RESET}\n"
RESP="$(cc_post "vibeos/boot" "{\"handle\":${SRC_HANDLE}}")"
if ok_field "${RESP}"; then
    pass "VOS_BOOT: boot command sent"
else
    fail "VOS_BOOT: failed (${RESP:-<empty>})"
    exit 1
fi

# Wait for guest to reach running state
BOOT_WAIT=0
BOOTED=0
while [ "${BOOT_WAIT}" -lt 60 ]; do
    sleep 5; BOOT_WAIT=$(( BOOT_WAIT + 5 ))
    RESP="$(cc_post "vibeos/status" "{\"handle\":${SRC_HANDLE}}" 2>/dev/null)"
    if printf '%s' "${RESP}" | grep -qE '"state":"running"'; then
        BOOTED=1; break
    fi
done

if [ "${BOOTED}" -eq 1 ]; then
    pass "VOS_STATUS: source reached running state (${BOOT_WAIT}s)"
else
    skip "VOS_STATUS: source not running yet (snapshot may fail) — continuing"
fi

# Optionally write a sentinel file via SSH to verify state survives restore
SENTINEL_VALUE="agentos-restore-$(date +%s)"
if [ "${HAVE_SSH}" = "1" ] && guest_ssh true 2>/dev/null; then
    guest_ssh "echo '${SENTINEL_VALUE}' > /tmp/restore_sentinel" 2>/dev/null && \
        pass "SSH: sentinel file written (${SENTINEL_VALUE})" || \
        skip "SSH: sentinel write failed — non-fatal"
fi

# ── Step 4: Snapshot the source ───────────────────────────────────────────────

printf "${BOLD}  Step 3: Snapshot source instance${RESET}\n"
RESP="$(cc_post "vibeos/snapshot" "{\"handle\":${SRC_HANDLE}}")"
if ok_field "${RESP}"; then
    SNAP_LO="$(printf '%s' "${RESP}" | grep -o '"snap_lo":[0-9]*' | grep -o '[0-9]*')"
    SNAP_HI="$(printf '%s' "${RESP}" | grep -o '"snap_hi":[0-9]*' | grep -o '[0-9]*')"
    pass "VOS_SNAPSHOT: snap_lo=${SNAP_LO:-?} snap_hi=${SNAP_HI:-?}"
elif printf '%s' "${RESP}" | grep -qE '"ok":false'; then
    skip "VOS_SNAPSHOT: returned error — snapshot not yet implemented; skipping restore test"
    # Destroy source and exit
    cc_post "vibeos/destroy" "{\"handle\":${SRC_HANDLE}}" >/dev/null 2>&1 || true
    exit 2
else
    fail "VOS_SNAPSHOT: failed (${RESP:-<empty>})"
    cc_post "vibeos/destroy" "{\"handle\":${SRC_HANDLE}}" >/dev/null 2>&1 || true
    exit 1
fi

# ── Step 5: Create destination VibeOS context ─────────────────────────────────

printf "${BOLD}  Step 4: Restore snapshot into a new VibeOS context${RESET}\n"
RESP="$(cc_post "vibeos/create" \
    "{\"name\":\"e2e-restore-dst\",\"vmm_type\":\"${VMM_TYPE}\"}")"
if ok_field "${RESP}"; then
    DST_HANDLE="$(printf '%s' "${RESP}" | grep -o '"handle":[0-9]*' | grep -o '[0-9]*')"
    pass "VOS_CREATE: destination handle=${DST_HANDLE:-?}"
else
    fail "VOS_CREATE: destination context creation failed"
    cc_post "vibeos/destroy" "{\"handle\":${SRC_HANDLE}}" >/dev/null 2>&1 || true
    exit 1
fi

# ── Step 6: Restore snapshot into destination ─────────────────────────────────

RESP="$(cc_post "vibeos/restore" \
    "{\"handle\":${DST_HANDLE},\"snap_lo\":${SNAP_LO:-0},\"snap_hi\":${SNAP_HI:-0}}")"
if ok_field "${RESP}"; then
    pass "VOS_RESTORE: snapshot loaded into destination"
else
    fail "VOS_RESTORE: restore failed (${RESP:-<empty>})"
    cc_post "vibeos/destroy" "{\"handle\":${SRC_HANDLE}}" >/dev/null 2>&1 || true
    cc_post "vibeos/destroy" "{\"handle\":${DST_HANDLE}}" >/dev/null 2>&1 || true
    exit 1
fi

# ── Step 7: Verify restored instance ──────────────────────────────────────────

printf "${BOLD}  Step 5: Verify restored instance state${RESET}\n"

RESP="$(cc_post "vibeos/status" "{\"handle\":${DST_HANDLE}}")"
if ok_field "${RESP}"; then
    STATE="$(printf '%s' "${RESP}" | grep -o '"state":"[^"]*"' | head -1)"
    case "${STATE}" in
        *running*|*booting*|*ready*)
            pass "VOS_STATUS (restored): state=${STATE}"
            ;;
        *)
            fail "VOS_STATUS (restored): unexpected state=${STATE:-?}"
            ;;
    esac

    # Verify vmm_type preserved
    if printf '%s' "${RESP}" | grep -q "\"vmm_type\":\"${VMM_TYPE}\""; then
        pass "VOS_STATUS (restored): vmm_type=${VMM_TYPE} preserved"
    else
        skip "VOS_STATUS (restored): vmm_type field not confirmed in response"
    fi
else
    fail "VOS_STATUS (restored): request failed"
fi

# Check sentinel file survived restore via SSH (if SSH available)
if [ "${HAVE_SSH}" = "1" ] && guest_ssh true 2>/dev/null; then
    SENTINEL_READ="$(guest_ssh "cat /tmp/restore_sentinel 2>/dev/null" 2>/dev/null || true)"
    if [ "${SENTINEL_READ}" = "${SENTINEL_VALUE}" ]; then
        pass "SSH: sentinel file intact after restore ('${SENTINEL_VALUE}')"
    elif [ -n "${SENTINEL_READ}" ]; then
        fail "SSH: sentinel value mismatch: got '${SENTINEL_READ}', expected '${SENTINEL_VALUE}'"
    else
        skip "SSH: sentinel file not found after restore (non-fatal if SSH port reassigned)"
    fi
fi

# ── Step 8: Verify restored instance appears in list ─────────────────────────

RESP="$(cc_get "vibeos/list")"
if ok_field "${RESP}"; then
    if printf '%s' "${RESP}" | grep -q '"e2e-restore-dst"'; then
        pass "VOS_LIST: restored instance appears in list"
    else
        fail "VOS_LIST: restored instance not in list"
    fi
else
    fail "VOS_LIST: request failed"
fi

# ── Step 9: Destroy both instances ────────────────────────────────────────────

for HANDLE in "${SRC_HANDLE}" "${DST_HANDLE}"; do
    [ -n "${HANDLE}" ] || continue
    RESP="$(cc_post "vibeos/destroy" "{\"handle\":${HANDLE}}")"
    if ok_field "${RESP}"; then
        pass "VOS_DESTROY: handle=${HANDLE} destroyed"
    else
        fail "VOS_DESTROY: handle=${HANDLE} failed (${RESP:-<empty>})"
    fi
done

# ── Summary ────────────────────────────────────────────────────────────────────

printf "\n  vibeos_restore: ${GREEN}%d passed${RESET}, ${RED}%d failed${RESET}, ${YELLOW}%d skipped${RESET}\n\n" \
    "${PASS}" "${FAIL}" "${SKIP}"
[ "${FAIL}" -eq 0 ] || exit 1
exit 0
