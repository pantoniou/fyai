#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a watcher attached to a driven session can submit input as a
# watcher message, and both driver and watcher observe the reply.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_jump_watch.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mkdir -p a/.git
(cd a && "$FYAI_BIN" init >/dev/null 2>&1 &&
	"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) || fail "cannot make the project"
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/ready" "$SYNC/done"
# Start the driver; one turn registers it, then it waits for the watcher reply.
(cd a && pty_run "$TEST_DIR/driver.out" "$FYAI_BIN" -b fleet/work -i 8<&- <<PTY

steps:
  - type: Hello
  - wait-screen: "Hi there."
    timeout: session
  - touch: "$SYNC/ready"
    timeout: session
  - wait-screen: "Heard the watcher."
    timeout: session
  - touch: "$SYNC/done"
    timeout: session
PTY
) &
driver=$!
read -r _ <"$SYNC/ready" || true

pty_run "$TEST_DIR/watcher.out" "$FYAI_BIN" fleet attach a:fleet/work 8<&- <<'PTY'
steps:
  - wait-screen: "Hi there."
    timeout: session
  - type: Please check the logs
  - wait-screen: "Heard the watcher."
    timeout: session
PTY
read -r _ <"$SYNC/done" || true
wait "$driver" || fail "the driver did not see the answer: $(tail -c 2000 "$TEST_DIR/driver.out")"
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the message did not reach the session"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
last = reqs[-1]["body"]["messages"][-1]["content"]
assert "[message 'a watcher']" in last and "Please check the logs" in last, last
PY
pass
