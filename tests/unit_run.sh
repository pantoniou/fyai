#!/bin/bash
# SPDX-License-Identifier: MIT
# unit_run.sh - run one unit test, optionally under a wrapper tool.
#
# CTest runs each registry entry through this script so that a run can put a
# tool in front of the test binary without reconfiguring the tree:
#
#   FYAI_VALGRIND="valgrind -q --error-exitcode=99" \
#       ctest --test-dir build -R 'fyai/unit'
#
# This is the variable tests/harness.sh applies to the functional cases, so
# one setting covers both suites. It is unquoted on purpose: it carries the
# tool and its options. Give the tool an error exit code. Without one it
# reports the memory errors but returns the status of the test, and the run
# passes.
#
# Contract (set by CMake/add_test):
#   argv[1] - path to the fyai_test binary
#   argv[2] - the <suite>/<name> to run
set -u

FYAI_TEST_BIN="${1:-}"
FYAI_TEST_NAME="${2:-}"

[ -x "$FYAI_TEST_BIN" ] || {
	echo "unit_run: test binary not set/executable" >&2
	exit 99
}

# Each test runs in a forked child of the binary, which the tool follows. Add
# --log-file=<dir>/vg.%p.txt to keep one log for each process.
exec ${FYAI_VALGRIND:-} "$FYAI_TEST_BIN" "$FYAI_TEST_NAME"
