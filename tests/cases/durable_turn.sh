#!/bin/bash
# SPDX-License-Identifier: MIT
# With `durable`, a process that is killed while its second model request is
# in flight leaves the first round on the branch: the call and its result.
# Without it, the turn is lost.
set -eu
. "$(dirname "$0")/../harness.sh"

for durable in true false; do
	fyai_test_setup
	mock_start durable_turn.json
	export MOCK_KEY=test-key
	"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
	    --set tools=true --set "durable=$durable" \
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
	    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
	printf 'one line\n' >data.txt
	mock_watch
	"$FYAI_BIN" -b main --color off "Start the long job" >out.txt 2>err.txt 8<&- &
	pid=$!
	wait_requests 2
	kill -9 "$pid"
	wait "$pid" 2>/dev/null || true
	"$FYAI_BIN" -b main export -o saved.md >/dev/null 2>&1 || fail "no export ($durable)"
	if [ "$durable" = true ]; then
		grep -q 'one line' saved.md || fail "the round was not stored"
	else
		! grep -q 'one line' saved.md || fail "the round was stored without durable"
	fi
	touch release-durable
	mock_stop
done

# The checkpoints of a finished turn leave no ref-log entry: main@{1} is the
# state before the turn, as without `durable`.
for durable in true false; do
	fyai_test_setup
	mock_start durable_turn.json
	export MOCK_KEY=test-key
	"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
	    --set tools=true --set "durable=$durable" \
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
	    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
	printf 'one line\n' >data.txt
	touch release-durable
	"$FYAI_BIN" -b main --color off "Start the long job" >out.txt 2>err.txt \
	    || fail "turn failed ($durable)"
	"$FYAI_BIN" -b main export -o prev.md 'main@{1}' >/dev/null 2>&1 \
	    || "$FYAI_BIN" -b "main@{1}" export -o prev.md >/dev/null 2>&1 \
	    || fail "no main@{1} ($durable)"
	! grep -q 'one line' prev.md || fail "main@{1} holds a round ($durable)"
	mock_stop
done
pass
