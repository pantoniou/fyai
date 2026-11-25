#!/bin/bash
# SPDX-License-Identifier: MIT
# A route that asks a decider: a `fyai decide` child chooses for each event.
# drop acknowledges it unseen, reply answers with the bound reply action of
# its kind, and wake gives the model the event with the decision beside it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_route_decide.json
EVENTS_LOG="$TEST_DIR/mcp-events.jsonl"
cat >"$TEST_DIR/on-list.jsonl" <<'JSON'
{"kind": "message", "id": "C1/1.1", "cursor": "c1", "data": {"user": "U0OWNER", "channel": "C1", "ts": "1.1", "text": "spam text"}}
{"kind": "message", "id": "C1/1.2", "cursor": "c2", "data": {"user": "U0OWNER", "channel": "C1", "ts": "1.2", "text": "question text"}}
{"kind": "message", "id": "C1/1.3", "cursor": "c3", "data": {"user": "U0OWNER", "channel": "C1", "ts": "1.3", "text": "build text"}}
JSON

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --set display/stream=false \
    --set api=chat-completions --set mcp/enabled=true \
    --set "decisions/inbox={use: triage, options: [drop, wake, defer, reply]}" \
    --set "mcp/servers/src={transport: stdio, command: '$PYTHON', args: ['$TESTS_DIR/mock/mock_mcp_events.py'], env: {MCP_EVENTS_LOG: '$EVENTS_LOG', MCP_EVENTS_ON_LIST: '$TEST_DIR/on-list.jsonl', MCP_EVENTS_MARKER: '$TEST_DIR/acked'}, cwd: '$TEST_DIR', events: true, principals: {U0OWNER: owner}, route: [{decide: inbox}]}" \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - wait: Woken.
    timeout: session
  - type: /exit
PTY

"$PYTHON" - "$TEST_DIR/requests.jsonl" "$EVENTS_LOG" <<'PY' || fail "the decisions of the route"
import json, sys
reqs = [json.loads(l)["body"] for l in open(sys.argv[1])]
deciding = [r for r in reqs if r.get("tool_choice") != "auto"]
assert len(deciding) == 3, [r.get("tool_choice") for r in reqs]
for r in deciding:
    assert [t["function"]["name"] for t in r["tools"]] == ["structured_output"]
    enum = r["tools"][0]["function"]["parameters"]["properties"]["decision"]["enum"]
    assert enum == ["drop", "wake", "defer", "reply"], enum
session = [r for r in reqs if r.get("tool_choice") == "auto"]
assert len(session) == 1, len(session)
events = [m["content"] for m in session[0]["messages"]
          if m.get("role") == "user" and str(m.get("content")).startswith("[event")]
woken, = events
assert woken.startswith("[event 'src/message'] C1/1.3 from U0OWNER (owner)"), woken
assert 'The decider inbox chose wake: {"summary"' in woken, woken
rows = [json.loads(l) for l in open(sys.argv[2])]
replies = [r["params"]["arguments"] for r in rows
           if r.get("method") == "tools/call" and r["params"]["name"] == "reply"]
assert replies == [{"text": "Answered by the decider.", "channel": "C1",
                    "thread_ts": "1.2"}], replies
acks = {r["params"]["id"]: r["params"] for r in rows
        if r.get("method") == "notifications/fyai/ack"}
assert acks["C1/1.1"] == {"id": "C1/1.1", "credits": 1, "dropped": True}, acks
assert acks["C1/1.2"] == {"id": "C1/1.2", "credits": 1, "answered": True}, acks
assert acks["C1/1.3"]["cursor"] == "c3", acks
PY

mock_stop 4
pass
