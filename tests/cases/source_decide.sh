#!/bin/bash
# SPDX-License-Identifier: MIT
# fyai source decide in front of an event source: a decider drops one event,
# which is acknowledged to the source and never reaches the client, and
# passes the other on with the decision, which the model reads after the data.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start source_decide.json
EVENTS_LOG="$TEST_DIR/mcp-events.jsonl"
cat >"$TEST_DIR/on-list.jsonl" <<'JSON'
{"kind": "message", "id": "C1/1.1", "cursor": "c1", "data": {"user": "U0OWNER", "channel": "C1", "ts": "1.1", "text": "spam text"}}
{"kind": "message", "id": "C1/1.2", "cursor": "c2", "data": {"user": "U0OWNER", "channel": "C1", "ts": "1.2", "text": "build text"}}
JSON

# The decider runs in the server, apart from the session: it reads what the
# project stores, and its key comes in the environment of the server.
run_fyai config set api chat-completions
assert_status 0
run_fyai config set api_url "$MOCK_URL/v1/chat/completions"
assert_status 0
run_fyai config set api_key '{type: env, value: MOCK_KEY}'
assert_status 0

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --set display/stream=false \
    --set mcp/enabled=true \
    --set "mcp/servers/src={transport: stdio, command: '$FYAI_BIN', args: [source, decide, --decider, triage, --model, mock-model, --, '$PYTHON', '$TESTS_DIR/mock/mock_mcp_events.py'], env: {MOCK_KEY: test-key, MCP_EVENTS_LOG: '$EVENTS_LOG', MCP_EVENTS_ON_LIST: '$TEST_DIR/on-list.jsonl'}, cwd: '$TEST_DIR', events: true, principals: {U0OWNER: owner}}" \
    -m mock-model -i <<'PTY'
timeout: 30
steps:
  - wait: Proxied.
    timeout: session
  - type: /exit
PTY

"$PYTHON" - "$TEST_DIR/requests.jsonl" "$EVENTS_LOG" <<'PY' || fail "the decisions of the proxy"
import json, sys
reqs = [json.loads(l)["body"] for l in open(sys.argv[1])]
session = [r for r in reqs if r.get("tool_choice") == "auto"]
assert len(session) == 1, [r.get("tool_choice") for r in reqs]
events = [m["content"] for m in session[0]["messages"]
          if m.get("role") == "user" and str(m.get("content")).startswith("[event")]
woken, = events
assert woken.startswith("[event 'src/message'] C1/1.2 from U0OWNER (owner)"), woken
assert 'The decider triage chose wake: {"summary"' in woken, woken
rows = [json.loads(l) for l in open(sys.argv[2])]
acks = {r["params"]["id"]: r["params"] for r in rows
        if r.get("method") == "notifications/fyai/ack"}
assert acks["C1/1.1"] == {"id": "C1/1.1", "credits": 1, "dropped": True}, acks
assert acks["C1/1.2"]["cursor"] == "c2", acks
PY

mock_stop 3
pass
