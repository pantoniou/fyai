#!/bin/bash
# SPDX-License-Identifier: MIT
# Slash checkout forks historical state; reset and rewind retain the old head.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai branch create source
assert_status 0
run_fyai -b source config set display/stream false
assert_status 0
run_fyai -b source config set display/stream true
assert_status 0

FYAI_PTY_INPUT="/checkout source" \
FYAI_PTY_NEEDLE="switched to branch source" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/checkout.out" \
	"$FYAI_BIN" -k test-key -m mock-model -b main -i

FYAI_PTY_INPUT="/checkout source@{2}" \
FYAI_PTY_NEEDLE="switched to branch session/" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/snapshot.out" \
	"$FYAI_BIN" -k test-key -m mock-model -b source -i

snapshot_name="$(grep -aoE 'created branch session/[0-9T.-]+' \
	"$TEST_DIR/snapshot.out" | head -1 | sed 's/^created branch //')"
[ -n "$snapshot_name" ] || fail "checkout did not name the snapshot branch"

run_fyai config get display/stream
assert_status 0
assert_stdout_contains "false"
run_fyai list reflog
assert_status 0
assert_stdout_contains "checkout"

FYAI_PTY_INPUT="/rewind source@{0}" \
FYAI_PTY_NEEDLE="the previous head is session/" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/rewind.out" \
	"$FYAI_BIN" -k test-key -m mock-model -b "$snapshot_name" -i

run_fyai config get display/stream
assert_status 0
assert_stdout_contains "false"
run_fyai list reflog
assert_status 0
assert_stdout_contains "reset"

FYAI_PTY_INPUT="/checkout -b named source@{2}" \
FYAI_PTY_NEEDLE="switched to branch named" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/named.out" \
	"$FYAI_BIN" -k test-key -m mock-model -b source -i

run_fyai -b named config get display/stream
assert_status 0
assert_stdout_contains "false"

FYAI_PTY_INPUT="/reset source@{0}" \
FYAI_PTY_NEEDLE="the previous head is named@{1}" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/reset.out" \
	"$FYAI_BIN" -k test-key -m mock-model -b named -i

run_fyai -b named list reflog
assert_status 0
assert_stdout_contains "reset"

pass
