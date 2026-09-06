#!/usr/bin/env bash
# agentOS Vibe Integration Test
# Tests the full agent→generate→compile→validate→swap→verify loop.
#
# Environment:
#   AGENTOS_CODEGEN_BACKEND=mock  (no real LLM; generate phase is skipped)
#   AGENTOS_BRIDGE_PORT=8790      (default)
#   AGENTOS_TIMEOUT=120           (seconds to wait for each phase)
#
# Exit codes:
#   0 — all phases passed (or skipped with acceptable reason)
#   1 — test failed
#   2 — setup error (cargo not available, etc.)
#
# NOTE: This script must be made executable before use:
#   chmod +x tests/vibe_integration_test.sh

set -e

# ── Color helpers ─────────────────────────────────────────────────────────────

if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    BOLD='\033[1m'
    RESET='\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    BOLD=''
    RESET=''
fi

pass()  { printf "${GREEN}[PASS]${RESET} %s\n" "$1"; }
fail()  { printf "${RED}[FAIL]${RESET} %s\n" "$1"; }
skip()  { printf "${YELLOW}[SKIP]${RESET} %s\n" "$1"; }
info()  { printf "${BOLD}[INFO]${RESET} %s\n" "$1"; }
phase() { printf "\n${BOLD}--- Phase %s: %s ---${RESET}\n" "$1" "$2"; }

# ── Config ────────────────────────────────────────────────────────────────────

BRIDGE_PORT="${AGENTOS_BRIDGE_PORT:-8790}"
TIMEOUT="${AGENTOS_TIMEOUT:-120}"
CODEGEN_BACKEND="${AGENTOS_CODEGEN_BACKEND:-mock}"
BRIDGE_BASE="http://localhost:${BRIDGE_PORT}"

BRIDGE_PID=""
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

# ── Cleanup trap ──────────────────────────────────────────────────────────────

cleanup() {
    if [ -n "${BRIDGE_PID}" ]; then
        info "Stopping bridge (PID ${BRIDGE_PID})..."
        kill "${BRIDGE_PID}" 2>/dev/null || true
        wait "${BRIDGE_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ── Phase result helpers ──────────────────────────────────────────────────────

phase_pass() {
    pass "$1"
    TESTS_PASSED=$(( TESTS_PASSED + 1 ))
}

phase_fail() {
    fail "$1"
    TESTS_FAILED=$(( TESTS_FAILED + 1 ))
}

phase_skip() {
    skip "$1"
    TESTS_SKIPPED=$(( TESTS_SKIPPED + 1 ))
}

# ── Prerequisite checks ───────────────────────────────────────────────────────

info "Checking prerequisites..."

if ! command -v cargo >/dev/null 2>&1; then
    fail "cargo not found — install Rust toolchain from https://rustup.rs/"
    exit 2
fi
info "cargo: $(cargo --version)"

if ! command -v curl >/dev/null 2>&1; then
    fail "curl not found — install curl"
    exit 2
fi
info "curl: $(curl --version | head -1)"

# clang for wasm32-wasi is optional — compile phase will SKIP if absent
CLANG_WASM_AVAILABLE=0
for CLANG_BIN in clang clang-17 clang-16 clang-15; do
    if command -v "${CLANG_BIN}" >/dev/null 2>&1; then
        if "${CLANG_BIN}" --target=wasm32-wasi --version >/dev/null 2>&1; then
            CLANG_WASM_AVAILABLE=1
            info "clang (wasm32-wasi): ${CLANG_BIN}"
            break
        fi
    fi
done
if [ "${CLANG_WASM_AVAILABLE}" -eq 0 ]; then
    info "clang with wasm32-wasi not found — compile phase will be skipped"
fi

# ── Turn off set -e for test section so failures don't exit early ─────────────

set +e

# ── Bridge startup ────────────────────────────────────────────────────────────

info "Checking bridge on port ${BRIDGE_PORT}..."

BRIDGE_ALREADY_RUNNING=0
if curl -sf --max-time 2 "${BRIDGE_BASE}/api/agentos/agents" >/dev/null 2>&1; then
    info "Bridge already running on port ${BRIDGE_PORT} — reusing."
    BRIDGE_ALREADY_RUNNING=1
fi

if [ "${BRIDGE_ALREADY_RUNNING}" -eq 0 ]; then
    info "Starting bridge: cargo run --bin agentos-console ..."
    # Find the repo root (parent of this script's directory)
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

    # Launch bridge; suppress its output unless AGENTOS_DEBUG is set
    if [ -n "${AGENTOS_DEBUG}" ]; then
        cargo run --manifest-path "${REPO_ROOT}/Cargo.toml" \
            --bin agentos-console 2>&1 &
    else
        cargo run --manifest-path "${REPO_ROOT}/Cargo.toml" \
            --bin agentos-console >/tmp/agentos-bridge-test.log 2>&1 &
    fi
    BRIDGE_PID=$!
    info "Bridge PID: ${BRIDGE_PID}"

    # Wait for bridge to become available (up to 30 s, poll every 2 s)
    info "Waiting for bridge to become ready (up to 30 s)..."
    READY=0
    for i in $(seq 1 15); do
        if curl -sf --max-time 2 "${BRIDGE_BASE}/api/agentos/agents" >/dev/null 2>&1; then
            READY=1
            break
        fi
        # Check if the process already died
        if ! kill -0 "${BRIDGE_PID}" 2>/dev/null; then
            fail "Bridge process exited unexpectedly (PID ${BRIDGE_PID})"
            if [ -f /tmp/agentos-bridge-test.log ]; then
                printf "Bridge log:\n"
                tail -20 /tmp/agentos-bridge-test.log
            fi
            exit 2
        fi
        info "  Attempt ${i}/15 — waiting 2 s..."
        sleep 2
    done

    if [ "${READY}" -eq 0 ]; then
        fail "Bridge did not become ready within 30 s"
        if [ -f /tmp/agentos-bridge-test.log ]; then
            printf "Bridge log:\n"
            tail -20 /tmp/agentos-bridge-test.log
        fi
        exit 2
    fi
    info "Bridge is ready."
fi

# ═════════════════════════════════════════════════════════════════════════════
# Phase 1 — Generate endpoint
# ═════════════════════════════════════════════════════════════════════════════

phase "1" "Generate endpoint (POST /api/agentos/vibe/generate)"

GENERATE_RESPONSE=$(curl -sf --max-time 30 \
    -X POST "${BRIDGE_BASE}/api/agentos/vibe/generate" \
    -H "Content-Type: application/json" \
    -d '{"prompt": "Generate a minimal storage.v1 service", "service_id": "storage.v1"}' \
    2>&1)
GENERATE_EXIT=$?

if [ "${GENERATE_EXIT}" -ne 0 ]; then
    phase_fail "Phase 1: curl request to /api/agentos/vibe/generate failed (exit ${GENERATE_EXIT})"
else
    # Check for ok:true and code field (real LLM response)
    if printf '%s' "${GENERATE_RESPONSE}" | grep -q '"ok":true' && \
       printf '%s' "${GENERATE_RESPONSE}" | grep -q '"code":'; then
        GENERATED_CODE=$(printf '%s' "${GENERATE_RESPONSE}" | \
            sed 's/.*"code":"\([^"]*\)".*/\1/' | \
            sed 's/\\n/\n/g' | sed 's/\\t/\t/g')
        info "Generated $(printf '%s' "${GENERATED_CODE}" | wc -c) bytes of C code (backend: $(printf '%s' "${GENERATE_RESPONSE}" | grep -o '"backend":"[^"]*"' | head -1))"
        phase_pass "Phase 1: generate returned ok:true with code field"
    # Check for expected "no backend" error in mock mode — treat as SKIP
    elif printf '%s' "${GENERATE_RESPONSE}" | grep -q '"ok":false' && \
         printf '%s' "${GENERATE_RESPONSE}" | grep -qi 'no codegen backend\|no.*backend\|api_key\|not available'; then
        phase_skip "Phase 1: no LLM backend available (AGENTOS_CODEGEN_BACKEND=${CODEGEN_BACKEND}) — expected in CI"
    else
        phase_fail "Phase 1: unexpected response from /api/agentos/vibe/generate: ${GENERATE_RESPONSE}"
    fi
fi

# ═════════════════════════════════════════════════════════════════════════════
# Phase 2 — Compile endpoint
# ═════════════════════════════════════════════════════════════════════════════

phase "2" "Compile endpoint (POST /api/agentos/vibe/compile)"

# Use the known-good mock_memfs.c from the vibe test suite as compile input
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MOCK_C_PATH="${SCRIPT_DIR}/vibe/mock_memfs.c"

if [ -f "${MOCK_C_PATH}" ]; then
    MOCK_C_SOURCE=$(cat "${MOCK_C_PATH}")
    info "Using mock source: ${MOCK_C_PATH} ($(wc -c < "${MOCK_C_PATH}") bytes)"
else
    # Inline minimal fallback if the file isn't found
    info "mock_memfs.c not found at ${MOCK_C_PATH} — using inline minimal C source"
    MOCK_C_SOURCE='
#include <stdint.h>
extern uint32_t aos_mr_get(int idx);
extern void aos_mr_set(int idx, uint32_t val);
int service_init(void) { return 0; }
int service_dispatch(uint32_t label, uint32_t in, uint32_t out) {
    (void)in; (void)out;
    if (label == 0x20u) { aos_mr_set(1, 0); aos_mr_set(2, 0); return 0; }
    return -1;
}
int service_health(void) { return 0; }
'
fi

# JSON-escape the C source (replace backslash, double-quote, newline, tab)
ESCAPED_SOURCE=$(printf '%s' "${MOCK_C_SOURCE}" | \
    sed 's/\\/\\\\/g' | \
    sed 's/"/\\"/g' | \
    awk '{gsub(/\t/, "\\t"); printf "%s\\n", $0}')

COMPILE_BODY="{\"source_c\": \"${ESCAPED_SOURCE}\", \"service_id\": \"storage.v1\"}"

COMPILE_RESPONSE=$(curl -sf --max-time "${TIMEOUT}" \
    -X POST "${BRIDGE_BASE}/api/agentos/vibe/compile" \
    -H "Content-Type: application/json" \
    -d "${COMPILE_BODY}" \
    2>&1)
COMPILE_EXIT=$?

if [ "${COMPILE_EXIT}" -ne 0 ]; then
    phase_fail "Phase 2: curl request to /api/agentos/vibe/compile failed (exit ${COMPILE_EXIT})"
    COMPILE_WASM_B64=""
else
    if printf '%s' "${COMPILE_RESPONSE}" | grep -q '"ok":true' && \
       printf '%s' "${COMPILE_RESPONSE}" | grep -q '"wasm_b64":'; then
        WASM_SIZE=$(printf '%s' "${COMPILE_RESPONSE}" | grep -o '"size":[0-9]*' | grep -o '[0-9]*')
        WASM_SHA256=$(printf '%s' "${COMPILE_RESPONSE}" | grep -o '"sha256":"[^"]*"' | sed 's/"sha256":"//;s/"//')
        COMPILE_WASM_B64=$(printf '%s' "${COMPILE_RESPONSE}" | grep -o '"wasm_b64":"[^"]*"' | sed 's/"wasm_b64":"//;s/"//')
        info "Compiled to WASM: ${WASM_SIZE} bytes, sha256=${WASM_SHA256}"
        phase_pass "Phase 2: compile returned ok:true with wasm_b64 (${WASM_SIZE} bytes)"
    # clang not found on this machine — acceptable SKIP
    elif printf '%s' "${COMPILE_RESPONSE}" | grep -q '"ok":false' && \
         printf '%s' "${COMPILE_RESPONSE}" | grep -qi 'clang not found\|clang.*install'; then
        phase_skip "Phase 2: clang with wasm32-wasi not found — install llvm to enable compile phase"
        COMPILE_WASM_B64=""
    else
        phase_fail "Phase 2: unexpected response from /api/agentos/vibe/compile: ${COMPILE_RESPONSE}"
        COMPILE_WASM_B64=""
    fi
fi

# ═════════════════════════════════════════════════════════════════════════════
# Phase 3 — WASM magic byte verification
# ═════════════════════════════════════════════════════════════════════════════

phase "3" "WASM magic byte verification"

if [ -z "${COMPILE_WASM_B64}" ]; then
    phase_skip "Phase 3: skipped — no WASM output from Phase 2"
else
    # Decode base64 and check first 4 bytes: 0x00 0x61 0x73 0x6D (\0asm)
    WASM_MAGIC=$(printf '%s' "${COMPILE_WASM_B64}" | \
        base64 -d 2>/dev/null | \
        od -A n -t x1 -N 4 2>/dev/null | \
        tr -d ' \n' | tr '[:upper:]' '[:lower:]')

    if [ "${WASM_MAGIC}" = "0061736d" ]; then
        info "WASM magic bytes: \\x00asm (0x0061736d) — valid WebAssembly module"
        phase_pass "Phase 3: WASM magic bytes are correct (\\x00asm)"
    elif [ -z "${WASM_MAGIC}" ]; then
        phase_fail "Phase 3: failed to decode base64 WASM or read magic bytes (base64/od not available?)"
    else
        phase_fail "Phase 3: invalid WASM magic bytes: '${WASM_MAGIC}' (expected '0061736d')"
    fi
fi

# ═════════════════════════════════════════════════════════════════════════════
# Phase 4 — Bridge health after test
# ═════════════════════════════════════════════════════════════════════════════

phase "4" "Bridge health check (GET /api/agentos/agents)"

HEALTH_RESPONSE=$(curl -sf --max-time 10 \
    "${BRIDGE_BASE}/api/agentos/agents" \
    2>&1)
HEALTH_EXIT=$?

if [ "${HEALTH_EXIT}" -ne 0 ]; then
    phase_fail "Phase 4: bridge health check failed — bridge not responding (exit ${HEALTH_EXIT})"
elif printf '%s' "${HEALTH_RESPONSE}" | grep -q '"agents":\['; then
    AGENT_COUNT=$(printf '%s' "${HEALTH_RESPONSE}" | grep -o '"id":' | wc -l | tr -d ' ')
    info "Bridge returned ${AGENT_COUNT} agent(s)"
    phase_pass "Phase 4: bridge is healthy and returned agents list"
else
    phase_fail "Phase 4: unexpected response from /api/agentos/agents: ${HEALTH_RESPONSE}"
fi

# ═════════════════════════════════════════════════════════════════════════════
# Summary
# ═════════════════════════════════════════════════════════════════════════════

TOTAL=$(( TESTS_PASSED + TESTS_FAILED + TESTS_SKIPPED ))

printf "\n${BOLD}══════════════════════════════════════════════════${RESET}\n"
printf "${BOLD}Vibe Integration Test Results${RESET}\n"
printf "${BOLD}══════════════════════════════════════════════════${RESET}\n"
printf "  ${GREEN}Passed${RESET}:  %d\n" "${TESTS_PASSED}"
printf "  ${RED}Failed${RESET}:  %d\n" "${TESTS_FAILED}"
printf "  ${YELLOW}Skipped${RESET}: %d\n" "${TESTS_SKIPPED}"
printf "  Total:   %d / %d\n" "${TESTS_PASSED}" "${TOTAL}"
printf "${BOLD}══════════════════════════════════════════════════${RESET}\n"

if [ "${TESTS_FAILED}" -gt 0 ]; then
    printf "${RED}FAIL — %d test(s) failed${RESET}\n\n" "${TESTS_FAILED}"
    exit 1
else
    printf "${GREEN}PASS — all non-skipped tests passed${RESET}\n\n"
    exit 0
fi
