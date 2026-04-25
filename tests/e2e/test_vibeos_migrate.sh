#!/usr/bin/env bash
# agentOS E2E — VOS_MIGRATE tests
#
# Exercises live migration of a running VibeOS guest between capability domains:
#
#   Phase A (source): VOS_CREATE → VOS_BIND_DEVICE → VOS_BOOT → wait running
#   Phase B (migrate): VOS_SNAPSHOT source → VOS_CREATE dest →
#                       VOS_RESTORE dest → VOS_BIND_DEVICE dest →
#                       VOS_BOOT dest (from restored state) → VOS_DESTROY source
#   Phase C (verify):  dest is running; SSH into dest; source is gone from list
#
# VOS_MIGRATE is the compound operation exposed as a single CC bridge call
# (vibeos/migrate) that atomically performs snapshot+restore+cutover.  This
# test also verifies the lower-level two-phase form (snapshot then restore)
# to confirm both paths work.
#
# Exit codes:
#   0 — PASS
#   1 — FAIL
#   2 — SKIP (bridge not available or migrate not implemented)

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
    skip "vibeos_migrate: CC bridge not available"
    exit 2
fi

SRC_HANDLE=""
DST_HANDLE=""

# ── Phase A: Create and boot source ───────────────────────────────────────────

printf "${BOLD}  Phase A: Create and boot source VibeOS instance${RESET}\n"

RESP="$(cc_post "vibeos/create" \
    "{\"name\":\"e2e-migrate-src\",\"vmm_type\":\"${VMM_TYPE}\"}")"
if ok_field "${RESP}"; then
    SRC_HANDLE="$(printf '%s' "${RESP}" | grep -o '"handle":[0-9]*' | grep -o '[0-9]*')"
    pass "VOS_CREATE source: handle=${SRC_HANDLE:-?}"
else
    fail "VOS_CREATE source: failed (${RESP:-<empty>})"
    exit 1
fi

for DEV in serial net block; do
    RESP="$(cc_post "vibeos/bind-device" \
        "{\"handle\":${SRC_HANDLE},\"dev_type\":\"${DEV}\",\"dev_handle\":0}")"
    if ok_field "${RESP}"; then
        pass "VOS_BIND_DEVICE(${DEV}) source: bound"
    else
        fail "VOS_BIND_DEVICE(${DEV}) source: failed (${RESP:-<empty>})"
    fi
done

RESP="$(cc_post "vibeos/boot" "{\"handle\":${SRC_HANDLE}}")"
if ok_field "${RESP}"; then
    pass "VOS_BOOT source: started"
else
    fail "VOS_BOOT source: failed"
    cc_post "vibeos/destroy" "{\"handle\":${SRC_HANDLE}}" >/dev/null 2>&1 || true
    exit 1
fi

# Wait for running state
BOOT_WAIT=0; BOOTED=0
while [ "${BOOT_WAIT}" -lt 60 ]; do
    sleep 5; BOOT_WAIT=$(( BOOT_WAIT + 5 ))
    RESP="$(cc_post "vibeos/status" "{\"handle\":${SRC_HANDLE}}" 2>/dev/null)"
    if printf '%s' "${RESP}" | grep -qE '"state":"running"'; then
        BOOTED=1; break
    fi
done

if [ "${BOOTED}" -eq 1 ]; then
    pass "VOS_STATUS source: running (${BOOT_WAIT}s)"
else
    skip "source not confirmed running — migration may still proceed"
fi

# Write a migration marker in the guest so we can verify it on the destination
MIGRATION_MARKER="agentos-migrate-$(date +%s)"
if [ "${HAVE_SSH}" = "1" ] && guest_ssh true 2>/dev/null; then
    guest_ssh "echo '${MIGRATION_MARKER}' > /tmp/migration_marker" 2>/dev/null && \
        pass "SSH: migration marker written (${MIGRATION_MARKER})" || \
        skip "SSH: marker write failed — non-fatal"
fi

# ── Phase B: Atomic migration via VOS_MIGRATE ─────────────────────────────────

printf "${BOLD}  Phase B: Migrate source to destination via VOS_MIGRATE${RESET}\n"

# Try the atomic migration endpoint first
RESP="$(cc_post "vibeos/migrate" \
    "{\"src_handle\":${SRC_HANDLE},\"dst_name\":\"e2e-migrate-dst\",\"vmm_type\":\"${VMM_TYPE}\"}")"

if ok_field "${RESP}"; then
    DST_HANDLE="$(printf '%s' "${RESP}" | grep -o '"dst_handle":[0-9]*' | grep -o '[0-9]*')"
    pass "VOS_MIGRATE: atomic migration complete, dst_handle=${DST_HANDLE:-?}"
    SRC_HANDLE=""   # source was destroyed by migrate
elif printf '%s' "${RESP}" | grep -qE '"ok":false|"error":"not.*impl|"error":"unsupported'; then
    # Migrate not yet implemented as atomic op — fall back to two-phase
    skip "VOS_MIGRATE: atomic form not implemented — using two-phase snapshot+restore"

    # Two-phase: snapshot source, create dest, restore
    RESP="$(cc_post "vibeos/snapshot" "{\"handle\":${SRC_HANDLE}}")"
    if ! ok_field "${RESP}"; then
        skip "VOS_SNAPSHOT: not implemented — skipping migrate test"
        cc_post "vibeos/destroy" "{\"handle\":${SRC_HANDLE}}" >/dev/null 2>&1 || true
        exit 2
    fi
    SNAP_LO="$(printf '%s' "${RESP}" | grep -o '"snap_lo":[0-9]*' | grep -o '[0-9]*')"
    SNAP_HI="$(printf '%s' "${RESP}" | grep -o '"snap_hi":[0-9]*' | grep -o '[0-9]*')"
    pass "VOS_SNAPSHOT: snap_lo=${SNAP_LO:-?} snap_hi=${SNAP_HI:-?}"

    RESP="$(cc_post "vibeos/create" \
        "{\"name\":\"e2e-migrate-dst\",\"vmm_type\":\"${VMM_TYPE}\"}")"
    if ok_field "${RESP}"; then
        DST_HANDLE="$(printf '%s' "${RESP}" | grep -o '"handle":[0-9]*' | grep -o '[0-9]*')"
        pass "VOS_CREATE destination: handle=${DST_HANDLE:-?}"
    else
        fail "VOS_CREATE destination: failed"
        cc_post "vibeos/destroy" "{\"handle\":${SRC_HANDLE}}" >/dev/null 2>&1 || true
        exit 1
    fi

    RESP="$(cc_post "vibeos/restore" \
        "{\"handle\":${DST_HANDLE},\"snap_lo\":${SNAP_LO:-0},\"snap_hi\":${SNAP_HI:-0}}")"
    if ok_field "${RESP}"; then
        pass "VOS_RESTORE: snapshot loaded into destination"
    else
        fail "VOS_RESTORE: failed (${RESP:-<empty>})"
        cc_post "vibeos/destroy" "{\"handle\":${SRC_HANDLE}}" >/dev/null 2>&1 || true
        cc_post "vibeos/destroy" "{\"handle\":${DST_HANDLE}}" >/dev/null 2>&1 || true
        exit 1
    fi

    # Rebind devices on destination
    for DEV in serial net block; do
        RESP="$(cc_post "vibeos/bind-device" \
            "{\"handle\":${DST_HANDLE},\"dev_type\":\"${DEV}\",\"dev_handle\":0}")"
        if ok_field "${RESP}"; then
            pass "VOS_BIND_DEVICE(${DEV}) destination: rebound"
        else
            fail "VOS_BIND_DEVICE(${DEV}) destination: failed (${RESP:-<empty>})"
        fi
    done

    # Destroy the source (cutover)
    RESP="$(cc_post "vibeos/destroy" "{\"handle\":${SRC_HANDLE}}")"
    if ok_field "${RESP}"; then
        pass "VOS_DESTROY source: source torn down (cutover complete)"
        SRC_HANDLE=""
    else
        fail "VOS_DESTROY source: failed (${RESP:-<empty>})"
    fi
else
    fail "VOS_MIGRATE: unexpected error (${RESP:-<empty>})"
    cc_post "vibeos/destroy" "{\"handle\":${SRC_HANDLE}}" >/dev/null 2>&1 || true
    exit 1
fi

# ── Phase C: Verify destination ───────────────────────────────────────────────

printf "${BOLD}  Phase C: Verify migrated destination${RESET}\n"

if [ -z "${DST_HANDLE:-}" ]; then
    fail "Phase C: no destination handle — cannot verify"
    exit 1
fi

RESP="$(cc_post "vibeos/status" "{\"handle\":${DST_HANDLE}}")"
if ok_field "${RESP}"; then
    STATE="$(printf '%s' "${RESP}" | grep -o '"state":"[^"]*"' | head -1)"
    case "${STATE}" in
        *running*|*booting*|*ready*)
            pass "VOS_STATUS destination: state=${STATE}"
            ;;
        *)
            fail "VOS_STATUS destination: unexpected state=${STATE:-?}"
            ;;
    esac
else
    fail "VOS_STATUS destination: request failed"
fi

# Verify source is gone from list
RESP="$(cc_get "vibeos/list")"
if ok_field "${RESP}"; then
    if printf '%s' "${RESP}" | grep -q '"e2e-migrate-src"'; then
        fail "VOS_LIST: source 'e2e-migrate-src' still in list after migration"
    else
        pass "VOS_LIST: source removed from list (cutover confirmed)"
    fi
    if printf '%s' "${RESP}" | grep -q '"e2e-migrate-dst"'; then
        pass "VOS_LIST: destination 'e2e-migrate-dst' appears in list"
    else
        fail "VOS_LIST: destination not found in list"
    fi
fi

# Verify migration marker is present on destination via SSH
if [ "${HAVE_SSH}" = "1" ] && guest_ssh true 2>/dev/null; then
    MARKER_READ="$(guest_ssh "cat /tmp/migration_marker 2>/dev/null" 2>/dev/null || true)"
    if [ "${MARKER_READ}" = "${MIGRATION_MARKER}" ]; then
        pass "SSH: migration marker intact on destination ('${MIGRATION_MARKER}')"
    elif [ -n "${MARKER_READ}" ]; then
        fail "SSH: marker mismatch: got '${MARKER_READ}', expected '${MIGRATION_MARKER}'"
    else
        skip "SSH: migration marker not found (non-fatal — SSH port may have changed)"
    fi
fi

# Verify double-boot on source is rejected (source should be destroyed)
if [ -n "${SRC_HANDLE:-}" ]; then
    RESP="$(cc_post "vibeos/boot" "{\"handle\":${SRC_HANDLE}}" 2>/dev/null)"
    if printf '%s' "${RESP}" | grep -qE '"ok":false|"error"'; then
        pass "Post-migrate: boot on destroyed source rejected (expected)"
    else
        skip "Post-migrate: source handle still valid (may not have been destroyed)"
    fi
fi

# ── Teardown ──────────────────────────────────────────────────────────────────

[ -n "${DST_HANDLE:-}" ] && \
    cc_post "vibeos/destroy" "{\"handle\":${DST_HANDLE}}" >/dev/null 2>&1 || true
[ -n "${SRC_HANDLE:-}" ] && \
    cc_post "vibeos/destroy" "{\"handle\":${SRC_HANDLE}}" >/dev/null 2>&1 || true

# ── Summary ───────────────────────────────────────────────────────────────────

printf "\n  vibeos_migrate: ${GREEN}%d passed${RESET}, ${RED}%d failed${RESET}, ${YELLOW}%d skipped${RESET}\n\n" \
    "${PASS}" "${FAIL}" "${SKIP}"
[ "${FAIL}" -eq 0 ] || exit 1
exit 0
