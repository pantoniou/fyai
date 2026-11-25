#!/bin/bash
# SPDX-License-Identifier: MIT
# An MCP stdio server with fyai/events: the capability, the listed kinds, a
# checked event for the model, rejections, a dropped resend, and the
# acknowledgement after the publish.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_events.json
EVENTS_LOG="$TEST_DIR/mcp-events.jsonl"

# A one-shot run takes no events during its turn, so drive a session.
pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --set display/stream=false \
    --set api=chat-completions --set mcp/enabled=true \
    --set "mcp/servers/src={transport: stdio, command: '$PYTHON', args: ['$TESTS_DIR/mock/mock_mcp_events.py'], env: {MCP_EVENTS_LOG: '$EVENTS_LOG'}, cwd: '$TEST_DIR', events: true}" \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: use src echo
  - wait: Events seen.
    timeout: session
  - type: /exit
PTY
grep -q "rejected" "$TEST_DIR/pty.out" || fail "the rejections were not shown"

# The model gets the good event and the note, framed as external data, and
# nothing of the rejected ones or the resend.
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the events did not reach the model as expected"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
text = "\n".join(m["content"] for m in reqs[1]["body"]["messages"]
                 if m.get("role") == "user" and isinstance(m.get("content"), str))
assert "[event 'src/message'] C0123/1.1" in text, text
assert '<external-data source="src" kind="message" id="C0123/1.1">' in text, text
assert "[event 'src/note'] note-1" in text, text
assert text.count("C0123/1.1") == 3, text        # header, frame, answer; once
assert "C0123/1.2" not in text and "C0123/1.3" not in text, text
# The data cannot close its frame.
assert text.count("</external-data>") == 2, text
assert "<\\/external-data>" in text, text
PY

"$PYTHON" - "$EVENTS_LOG" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
methods = [r.get("method") for r in rows]
init = rows[0]
assert init["method"] == "initialize"
offer = init["params"]["capabilities"]["experimental"]["fyai/events"]
assert offer["version"] == 1 and offer["credits"] > 0, offer
# Both pages of kinds are listed after the tools.
lists = [r for r in rows if r.get("method") == "fyai/events/list"]
assert [l["params"].get("cursor") for l in lists] == [None, "page-2"], lists
assert methods.index("tools/list") < methods.index("fyai/events/list")
rejected = [r["params"] for r in rows
            if r.get("method") == "notifications/fyai/rejected"]
assert [r["id"] for r in rejected] == ["C0123/1.2", "C0123/1.3"], rejected
assert any("text" in p for p in rejected[0]["problems"]), rejected[0]
assert "unknown kind" in rejected[1]["problems"][0], rejected[1]
# The server's ping is answered.
assert any(r.get("id") == "server-ping" and r.get("result") == {}
           for r in rows), "the ping was not answered"
acks = [r["params"] for r in rows if r.get("method") == "notifications/fyai/ack"]
assert [(a.get("id"), a.get("cursor")) for a in acks] == [
    ("C0123/1.1", "cursor-1"), ("note-1", "cursor-4")], acks
assert all(a["credits"] == 1 for a in acks), acks
assert rows[-1] == {"eof": True}
PY

mock_stop 2
pass
