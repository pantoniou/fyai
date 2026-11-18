#!/bin/bash
# SPDX-License-Identifier: MIT
# With `durable`, a named wait and a monitor outlive a process that is killed:
# the branch holds them, and an interactive start on it opens the wait again
# and restarts the monitor, then tells the model.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start durable_pending.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true --set durable=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mock_watch
"$FYAI_BIN" -b main --color off "Start the jobs" >out.txt 2>err.txt 8<&- &
pid=$!
wait_requests 2
kill -9 "$pid"
wait "$pid" 2>/dev/null || true


pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set durable=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - wait: Picked up the wait.
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'EOF' || fail "the restored work did not reach the model"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
msgs = reqs[-1]["body"]["messages"]
users = "\n".join(m["content"] for m in msgs
                  if m.get("role") == "user" and isinstance(m.get("content"), str))
for want in ("wait 'later' fired", "check the thing", "monitor 'mon' restarted"):
    if want not in users:
        raise SystemExit("missing %r in %r" % (want, users))
EOF
touch release-pending
mock_stop

# A run that ends by itself takes its table with it: nothing is restored, so
# the first request after a start on the branch is the one the user types.
fyai_test_setup
mock_start durable_pending_clean.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true --set durable=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
pty_run "$TEST_DIR/pty1.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set durable=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: Start the monitor
  - wait: All set.
    timeout: session
PTY
pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set durable=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: PING-NOW
  - wait: Pong.
    timeout: session
PTY
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'EOF' || fail "a clean end left work behind"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
body = json.dumps(reqs[-1]["body"]["messages"])
if "restarted" in body:
    raise SystemExit("the monitor was restarted after a clean end")
EOF
mock_stop
pass
