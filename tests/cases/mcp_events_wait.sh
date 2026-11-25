#!/bin/bash
# SPDX-License-Identifier: MIT
# An event source in the live view: `wait` with `for` returns with the next
# event of a source, `list` with kind `sources` gives its state and counts,
# and display/live_sources gives it a line in the live panel.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_events_wait.json
EVENTS_LOG="$TEST_DIR/mcp-events.jsonl"
mkfifo "$TEST_DIR/events.fifo"

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --set display/stream=false \
    --set api=chat-completions --set mcp/enabled=true \
    --set display/live_sources=true \
    --set "mcp/servers/src={transport: stdio, command: '$PYTHON', args: ['$TESTS_DIR/mock/mock_mcp_events.py'], env: {MCP_EVENTS_LOG: '$EVENTS_LOG', MCP_EVENTS_FIFO: '$TEST_DIR/events.fifo'}, cwd: '$TEST_DIR', events: true}" \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: wait for src
  - wait: Sources listed.
    timeout: session
  - type: /exit
PTY
grep -a -q 'src.* in, [0-9]* done, [0-9]* rejected' "$TEST_DIR/pty.out" ||
	fail "the source has no line in the live panel"

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the wait or the list of sources"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
def tool(req, call):
    for m in req["body"]["messages"]:
        if m.get("role") == "tool" and m.get("tool_call_id") == call:
            return m["content"]
    raise SystemExit("no result for %s" % call)
waited = tool(reqs[1], "call_wait")
assert waited.startswith("[event 'src/note'] fifo-1"), waited
assert '"poked"' in waited, waited
listed = json.loads(tool(reqs[2], "call_list"))
row, = listed["sources"]
assert row["name"] == "src" and row["state"] == "up", row
assert row["received"] == 1 and row["rejected"] == 0, row
assert row["resent"] == 0 and row["kinds"] == 2, row
assert row["last"] == "note: poked", row
# The event went to the wait, not to the model a second time.
assert not any(m.get("role") == "user" and "fifo-1" in str(m.get("content"))
               for m in reqs[2]["body"]["messages"]), "delivered twice"

PY

mock_stop 3
pass
