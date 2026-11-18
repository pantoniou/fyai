#!/bin/bash
# SPDX-License-Identifier: MIT
# With `durable`, a background sub-agent that has a sub-agent of its own outlives
# a session that is killed: each runs again from its last stored step, and each
# parent is told.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start durable_agent_nested.json
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
wait_requests 6
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

# A start on the branch runs the sub-agent again, and the sub-agent runs its
# own sub-agent again from the round that it stored. The inner one ends, the
# sub-agent ends, and the report reaches the model.
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

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'EOF' || fail "the inner sub-agent did not go on"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
texts = [json.dumps(r["body"]["messages"]) for r in reqs[6:]]
inner = [t for t in texts if "INNER-OUT" in t and "you start again" in t]
if not inner:
    raise SystemExit("the inner sub-agent was not started again")
if not any("agent 'inner' restarted" in t for t in texts):
    raise SystemExit("the sub-agent was not told")
EOF
# The two requests that the lost session left held at the mock end with it.
touch release-agent
mock_stop
pass
