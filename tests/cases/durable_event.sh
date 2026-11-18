#!/bin/bash
# SPDX-License-Identifier: MIT
# With `durable`, an event that waits for the model is stored with the turn: a
# wait fires while the turn runs, the process is killed before the model takes
# the event, and the next start gives it to the model.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start durable_event.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true --set durable=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mock_watch
"$FYAI_BIN" -b main --color off "Fire the wait" >out.txt 2>err.txt 8<&- &
pid=$!
wait_requests 3
kill -9 "$pid"
wait "$pid" 2>/dev/null || true

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set durable=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - wait: Got the stored event.
    timeout: session
PTY
touch release-event
mock_stop
pass
