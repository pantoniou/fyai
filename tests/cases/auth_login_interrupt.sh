#!/bin/bash
# SPDX-License-Identifier: MIT
# A browser login waits on the application loop and must remain cancellable.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

"$FYAI_BIN" -k test-key --color off auth login --no-browser \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null &
pid=$!

i=0
while ! grep -q "Open this URL" "$TEST_DIR/stdout"; do
	kill -0 "$pid" 2>/dev/null ||
		fail "auth login exited before opening the receiver"
	i=$((i + 1))
	[ "$i" -lt 100 ] || fail "auth login did not start"
	sleep 0.05
done

kill -INT "$pid"
set +e
wait "$pid"
status=$?
set -e

[ "$status" -ne 0 ] || fail "interrupted auth login succeeded"
assert_stderr_contains "login failed"

pass
