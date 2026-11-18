#!/bin/bash
# SPDX-License-Identifier: MIT
# With `durable`, a background sub-agent outlives a session that is killed: its
# conversation is on its branch, and a start on the parent branch runs it again
# from the last stored step and tells the model.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start durable_agent.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true --set durable=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mock_watch

# The session starts the sub-agent, which runs one round and waits for the
# release of its second request. Then every process of the session is killed.
(pty_run "$TEST_DIR/pty1.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set durable=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 60
steps:
  - type: Run the helper
  - wait: Started.
    timeout: session
  - wait: NEVER-ON-SCREEN
    timeout: session
PTY
) >/dev/null 2>&1 &
driver=$!
wait_requests 4
url="$MOCK_URL/v1/chat/completions"
# Every process of the session, found from the clients that name this mock.
tree=$(ps -eo pid=,args= | grep -F -- "api_url=$url" | grep -v grep | awk '{print $1}')
all=$tree
while [ -n "$tree" ]; do
	next=
	for p in $tree; do next="$next $(pgrep -P "$p" || true)"; done
	tree=$(echo $next)
	all="$all $tree"
done
kill -9 $all 2>/dev/null || true
wait "$driver" 2>/dev/null || true

"$FYAI_BIN" branch --all --output json >branches.txt 2>&1
grep -q "agent:helper" branches.txt || fail "the sub-agent kept no branch"

# A start on the branch runs the sub-agent again, from its stored conversation.
pty_run "$TEST_DIR/pty2.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set durable=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 60
steps:
  - wait: Got the report.
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'EOF' || fail "the sub-agent did not go on"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
texts = [json.dumps(r["body"]["messages"]) for r in reqs]
again = [t for t in texts[4:] if "The session was lost and you start again" in t]
if not again:
    raise SystemExit("the sub-agent was not started again")
if "ROUND-ONE-OUT" not in again[0]:
    raise SystemExit("the sub-agent lost its stored round")
if not any("agent 'helper' restarted" in t for t in texts[4:]):
    raise SystemExit("the model was not told")
EOF
mock_stop
pass
