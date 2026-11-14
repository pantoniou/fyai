#!/bin/bash
# SPDX-License-Identifier: MIT
# A request that waits out a backoff has no transfer in flight, so the wait is
# the one place a turn can hang with nothing to cancel. Ctrl-C must end it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start retry_interrupt.json

# A backoff far longer than this case may take: the interrupt has to end the
# wait, not outlast it.
# The run writes its report to a FIFO that this case reads: the read wakes when
# the report is written, which is when the timer is armed. End of file means
# that the run exited without waiting.
mkfifo "$TEST_DIR/stderr.fifo" || fail "cannot make the FIFO"
"$FYAI_BIN" -k test-key --color off --set api=chat-completions --set tools=false \
	--set retry/max_attempts=4 --set retry/initial_delay_ms=600000 \
	--set retry/max_delay_ms=600000 \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello" \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr.fifo" </dev/null &
pid=$!
exec 8<"$TEST_DIR/stderr.fifo"
: >"$TEST_DIR/stderr"
armed=0
while [ "$armed" = 0 ] && IFS= read -r -u 8 line; do
	printf '%s\n' "$line" >>"$TEST_DIR/stderr"
	case "$line" in *"retrying in"*) armed=1 ;; esac
done
[ "$armed" = 1 ] || fail "the run exited before it waited"
# Keep reading, or the run would write to a FIFO that nobody holds.
cat <&8 >>"$TEST_DIR/stderr" &
drain=$!
exec 8<&-

start=$SECONDS
kill -INT "$pid"
set +e
wait "$pid"
status=$?
wait "$drain"
set -e
[ "$status" -ne 0 ] || fail "the interrupted run reported success"
# It must end because of the signal, not because the backoff expired.
[ $((SECONDS - start)) -lt $((30 * FYAI_TIMEOUT_SCALE)) ] || fail "the interrupt did not end the wait"

mock_stop 1
pass
