#!/bin/sh
# run_api_tests.sh — compile and run all agentOS API tests, emit TAP output
#
# Usage:
#   sh tests/api/run_api_tests.sh        # run all tests
#   sh tests/api/run_api_tests.sh -v     # verbose (show each test name)
#
# Exit code: 0 if all suites pass, non-zero otherwise.
#
# This script is called by `cargo xtask test-api`.  It can also be piped
# through `prove` or any other TAP harness:
#   prove -e sh tests/api/run_api_tests.sh
#
# Copyright (c) 2026 The agentOS Project
# SPDX-License-Identifier: BSD-2-Clause

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TESTS_DIR="${REPO_ROOT}/tests/api"
BUILD_DIR="${TMPDIR:-/tmp}/agentos-api-tests-$$"

CC="${CC:-cc}"
CFLAGS="-DAGENTOS_TEST_HOST -I${TESTS_DIR} -I${REPO_ROOT}/kernel/agentos-root-task/include -std=c11 -Wall -Wextra -Wpedantic"

mkdir -p "${BUILD_DIR}"

# List every test file explicitly so new files must be added intentionally.
TEST_FILES="
    test_msgbus.c
    test_capstore.c
    test_memfs.c
    test_logsvc.c
    test_vibeos.c
    test_cap_accounting.c
    test_ut_alloc.c
    test_ep_alloc.c
    test_pd_vspace.c
    test_pd_tcb.c
    test_nameserver.c
    test_storage_stack.c
"

PASSED=0
FAILED=0
ERRORS=0

for f in $TEST_FILES; do
    src="${TESTS_DIR}/${f}"
    bin="${BUILD_DIR}/${f%.c}"
    suite="${f%.c}"

    # Compile
    if ! ${CC} ${CFLAGS} -o "${bin}" "${src}" 2>"${bin}.stderr"; then
        printf "BAIL OUT! Compile error in %s:\n" "${f}"
        cat "${bin}.stderr"
        rm -rf "${BUILD_DIR}"
        exit 1
    fi

    # Run and capture output + exit code
    if "${bin}" >"${bin}.tap" 2>&1; then
        suite_exit=0
    else
        suite_exit=$?
    fi

    # Count TAP results from this suite
    ok_count=$(grep -c '^ok ' "${bin}.tap" 2>/dev/null || true)
    notok_count=$(grep -c '^not ok ' "${bin}.tap" 2>/dev/null || true)

    printf "# Suite: %s  ok=%s  not_ok=%s\n" \
        "${suite}" "${ok_count}" "${notok_count}"

    if [ "${suite_exit}" -eq 0 ]; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
        # Echo failing test lines to stderr for visibility
        grep '^not ok' "${bin}.tap" >&2 || true
    fi
done

rm -rf "${BUILD_DIR}"

echo ""
echo "# === agentOS API test summary ==="
echo "# Suites passed : ${PASSED}"
echo "# Suites failed : ${FAILED}"
echo "# Compile errors: ${ERRORS}"

if [ "${FAILED}" -gt 0 ] || [ "${ERRORS}" -gt 0 ]; then
    echo "# RESULT: FAIL"
    exit 1
fi

echo "# RESULT: PASS"
exit 0
