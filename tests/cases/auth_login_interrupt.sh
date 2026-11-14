#!/bin/bash
# SPDX-License-Identifier: MIT
# A browser login waits on the application loop and must remain cancellable.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# The run writes to a FIFO that this case reads: the read wakes when the link
# is written. End of file means that the run exited before it opened the
# receiver.
mkfifo "$TEST_DIR/stdout.fifo" || fail "cannot make the FIFO"
"$FYAI_BIN" -k test-key --color off auth login --no-browser \
	>"$TEST_DIR/stdout.fifo" 2>"$TEST_DIR/stderr" </dev/null &
pid=$!
exec 8<"$TEST_DIR/stdout.fifo"
: >"$TEST_DIR/stdout"
opened=0
while [ "$opened" = 0 ] && IFS= read -r -u 8 line; do
	printf '%s\n' "$line" >>"$TEST_DIR/stdout"
	case "$line" in *"Open this link"*) opened=1 ;; esac
done
[ "$opened" = 1 ] || fail "auth login exited before opening the receiver"
# Keep reading, or the run would write to a FIFO that nobody holds.
cat <&8 >>"$TEST_DIR/stdout" &
drain=$!
exec 8<&-

kill -INT "$pid"
set +e
wait "$pid"
status=$?
wait "$drain"
set -e

[ "$status" -ne 0 ] || fail "interrupted auth login succeeded"
assert_stderr_contains "login failed"

pass
