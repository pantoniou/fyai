#!/bin/bash
# SPDX-License-Identifier: MIT
# Trust and routes of an event source: the principal of a kind maps to a
# trust level, an event of anyone but the owner is marked as data, and a
# rule of the route drops an event, which is acknowledged without a cursor.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_trust.json
EVENTS_LOG="$TEST_DIR/mcp-events.jsonl"

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --set display/stream=false \
    --set api=chat-completions --set mcp/enabled=true \
    --set "mcp/servers/src={transport: stdio, command: '$PYTHON', args: ['$TESTS_DIR/mock/mock_mcp_events.py'], env: {MCP_EVENTS_LOG: '$EVENTS_LOG', MCP_EVENTS_STRANGER: '1'}, cwd: '$TEST_DIR', events: true, principals: {U0OWNER: owner}, route: [{when: {properties: {data: {type: object, properties: {text: {const: spam}}, required: [text]}}}, do: drop}]}" \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: use src echo
  - wait: Trusted.
    timeout: session
  - type: /exit
PTY

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the trust of the events"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
events = [m["content"] for m in reqs[1]["body"]["messages"]
          if m.get("role") == "user" and str(m.get("content")).startswith("[event")]
owner, = [e for e in events if "C0123/1.1" in e.split("\n")[0]]
stranger, = [e for e in events if "C0123/2.1" in e.split("\n")[0]]
note, = [e for e in events if "note-1" in e.split("\n")[0]]
assert owner.startswith("[event 'src/message'] C0123/1.1 from U0OWNER (owner)\n"), owner
assert "not the owner" not in owner, owner
assert stranger.startswith("[event 'src/message'] C0123/2.1 from U0STRANGER (anyone)\n"), stranger
assert "not the owner (trust: anyone)" in stranger, stranger
# A kind without a principal has the trust of the server: known.
assert "not the owner (trust: known)" in note, note
assert not any("C0123/2.2" in e for e in events), events
PY

"$PYTHON" - "$EVENTS_LOG" <<'PY' || fail "the acknowledgements"
import json, sys
acks = [r["params"] for r in map(json.loads, open(sys.argv[1]))
        if r.get("method") == "notifications/fyai/ack"]
dropped = [a for a in acks if a.get("id") == "C0123/2.2"]
assert dropped == [{"id": "C0123/2.2", "credits": 0, "dropped": True}], acks
assert {a["id"] for a in acks if "cursor" in a} == {
    "C0123/1.1", "C0123/2.1", "note-1"}, acks
PY

mock_stop 2
pass
