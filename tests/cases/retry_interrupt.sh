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
"$FYAI_BIN" -k test-key --color off --set api=chat-completions --set tools=false \
	--set retry/max_attempts=4 --set retry/initial_delay_ms=600000 \
	--set retry/max_delay_ms=600000 \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello" \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null &
pid=$!

# Wait for the backoff to be armed: the report is written when the timer is.
i=0
while ! grep -q "retrying in" "$TEST_DIR/stderr" 2>/dev/null; do
	kill -0 "$pid" 2>/dev/null || fail "the run exited before it waited"
	i=$((i + 1))
	[ "$i" -lt 200 ] || fail "the backoff was never armed"
	sleep 0.05
done

start=$SECONDS
kill -INT "$pid"
set +e
wait "$pid"
status=$?
set -e
[ "$status" -ne 0 ] || fail "the interrupted run reported success"
# It must end because of the signal, not because the backoff expired.
[ $((SECONDS - start)) -lt 30 ] || fail "the interrupt did not end the wait"

mock_stop 1
pass
