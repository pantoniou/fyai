#!/bin/bash
# SPDX-License-Identifier: MIT
# Bound actions: a tool that a kind binds is offered without the bound
# arguments and with `event`; a call fills them from the event, defaults to
# the one open event, and refuses an unknown or an ambiguous one.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_actions.json
EVENTS_LOG="$TEST_DIR/mcp-events.jsonl"

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --set display/stream=false \
    --set api=chat-completions --set mcp/enabled=true \
    --set "mcp/servers/src={transport: stdio, command: '$PYTHON', args: ['$TESTS_DIR/mock/mock_mcp_events.py'], env: {MCP_EVENTS_LOG: '$EVENTS_LOG'}, cwd: '$TEST_DIR', events: true}" \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: answer the chat
  - wait: Replied.
    timeout: session
  - type: /exit
PTY

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the bound action as the model sees it"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
tool, = [t["function"] for t in reqs[0]["body"]["tools"]
         if t["function"]["name"] == "mcp__src__reply"]
props = tool["parameters"]["properties"]
assert set(props) == {"text", "event"}, props
assert tool["parameters"]["required"] == ["text"], tool["parameters"]
assert "message" in tool["description"], tool["description"]
users = "\n".join(m["content"] for m in reqs[1]["body"]["messages"]
                  if m.get("role") == "user" and isinstance(m.get("content"), str))
assert 'Answer with mcp__src__reply and event "C0123/1.1"' in users, users
def result(req, call):
    for m in req["body"]["messages"]:
        if m.get("tool_call_id") == call:
            return m["content"]
    raise SystemExit("no result for " + call)
assert result(reqs[2], "call_reply") == "replied in C0123/1.1", result(reqs[2], "call_reply")
again = result(reqs[3], "call_again")
assert again.startswith("tool error: no event that reply answers is open"), again
assert result(reqs[3], "call_named") == "replied in C0123/1.1"
bad = result(reqs[3], "call_bad")
assert bad.startswith("tool error: no event 'nope'"), bad
PY

"$PYTHON" - "$EVENTS_LOG" <<'PY' || fail "what the server was called with"
import json, sys
calls = [r["params"]["arguments"] for r in map(json.loads, open(sys.argv[1]))
         if r.get("method") == "tools/call" and r["params"]["name"] == "reply"]
assert calls == [
    {"text": "Done.", "channel": "C0123", "thread_ts": "1.1"},
    {"text": "Named.", "channel": "C0123", "thread_ts": "1.1"}], calls
PY

mock_stop 4
pass
