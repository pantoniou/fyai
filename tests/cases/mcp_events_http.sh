#!/bin/bash
# SPDX-License-Identifier: MIT
# An MCP server over HTTP with fyai/events: its events come on the stream that
# fyai keeps open to it (a GET of server-sent events) and within the answer
# of a call. fyai answers its ping with a POST, opens the stream again with
# the id of the last event when the server closes it, rejects an event that
# fails its schema, and acknowledges the others after the publish. A server
# that refuses the stream sends no events.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
EVENTS_LOG="$TEST_DIR/mcp-events.jsonl"

# Start the server and read its port; the read waits for it.
http_server() {
	mkfifo "$TEST_DIR/mcp-port"
	env "$@" MCP_EVENTS_LOG="$EVENTS_LOG" MCP_HTTP_PORT="$TEST_DIR/mcp-port" \
		"$PYTHON" "$TESTS_DIR/mock/mock_mcp_events_http.py" &
	server=$!
	fyai_test_on_exit "kill $server 2>/dev/null || true"
	read -r port <"$TEST_DIR/mcp-port"
	rm -f "$TEST_DIR/mcp-port"
}

mock_start mcp_events.json
http_server
pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --set display/stream=false \
    --set api=chat-completions --set mcp/enabled=true \
    --set "mcp/servers/src={endpoint: 'http://127.0.0.1:$port/mcp', events: true}" \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: use src echo
  - wait: Events seen.
    timeout: session
  - type: /exit
PTY
grep -q "rejected" "$TEST_DIR/pty.out" || fail "the rejection was not shown"
mock_stop 2

# The model gets the three good events, framed as external data.
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the events did not reach the model as expected"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
text = "\n".join(m["content"] for m in reqs[1]["body"]["messages"]
                 if m.get("role") == "user" and isinstance(m.get("content"), str))
assert "[event 'src/message'] C0123/1.1" in text, text
assert "[event 'src/note'] note-2" in text, text
assert "[event 'src/note'] note-3" in text, text
assert "C0123/1.2" not in text, text
PY

"$PYTHON" - "$EVENTS_LOG" <<'PY' || fail "the server did not see what it should"
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert not any("timeout" in r for r in rows), rows
gets = [r for r in rows if r.get("get")]
# The stream opened, and opened again after the last event it delivered.
assert len(gets) >= 2, gets
assert all(g["accept"] == "text/event-stream" for g in gets), gets
assert all(g["session"] == "sess-1" for g in gets), gets
assert gets[0]["last_event_id"] is None, gets
assert gets[1]["last_event_id"] == "3", gets
# The stream opens once the kinds are listed.
first_get = rows.index(gets[0])
assert any(r.get("method") == "fyai/events/list" for r in rows[:first_get]), rows
# Both pings were answered with a POST.
for ping in ("server-ping", "server-ping-2"):
    assert any(r.get("id") == ping and r.get("result") == {} for r in rows), ping
rejected = [r["params"] for r in rows
            if r.get("method") == "notifications/fyai/rejected"]
assert [r["id"] for r in rejected] == ["C0123/1.2"], rejected
acks = sorted((r["params"].get("id"), r["params"].get("cursor")) for r in rows
              if r.get("method") == "notifications/fyai/ack")
assert acks == [("C0123/1.1", "cursor-1"), ("note-2", "cursor-4"),
                ("note-3", "cursor-5")], acks
PY

# A server that refuses the stream: the session says so and goes on.
kill "$server" 2>/dev/null || true
wait "$server" 2>/dev/null || true
http_server MCP_HTTP_NO_STREAM=1
pty_run "$TEST_DIR/pty-refused.out" \
    "$FYAI_BIN" -k test-key --set display/stream=false \
    --set api=chat-completions --set mcp/enabled=true \
    --set "mcp/servers/src={endpoint: 'http://127.0.0.1:$port/mcp', events: true}" \
    --set "api_url=http://127.0.0.1:1/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - wait-screen: "refused the stream of its events"
    timeout: session
  - type: /exit
PTY
pass
