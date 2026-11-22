#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that with `durable`, nested background sub-agents survive a killed
# session: each resumes from its last checkpoint and notifies its parent.
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

# Start the nested sub-agents, let them block on their held requests, then kill
# every process in the session.
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
# Find all session processes via the mock URL argument and kill their tree.
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

# Restarting on the parent branch resumes helper, which resumes inner from its
# checkpoint; both finish and deliver their reports up the chain.
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
# Release the two requests left blocked at the mock by the killed session.
touch release-agent
mock_stop
pass
