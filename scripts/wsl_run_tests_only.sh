#!/usr/bin/env bash
# Run full prism_tests on Linux ext4 (see ~/prism-run). No doctest filters.
set -euo pipefail
pkill -9 prism_tests 2>/dev/null || true
DEST="${PRISM_WSL_DIR:-${HOME}/prism-run}"
BUILD="${DEST}/build"
LOG="${DEST}/prism_tests_full.log"
EXIT="${DEST}/prism_tests_exit.txt"
XML="${DEST}/prism_tests.xml"
rm -f "${LOG}" "${EXIT}" "${XML}"
cd "${BUILD}"
./prism_tests -r=xml -o "${XML}" > "${LOG}" 2>&1
echo $? > "${EXIT}"
grep -E '^\[doctest\]|test cases:|assertions:|Status:' "${LOG}" | tail -25 || true
grep -E 'FAILED!|ERROR: TEST CASE' "${LOG}" | head -40 || true
