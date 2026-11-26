#!/bin/bash
# SPDX-License-Identifier: MIT
# A configured event source over stdio whose process ends starts again: the
# session says so, the source goes through its handshake again with the
# cursor fyai acknowledged, and its events reach the model as before.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_source_restart.json
EVENTS_LOG="$TEST_DIR/mcp-events.jsonl"
mkfifo "$TEST_DIR/events.fifo"

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --set display/stream=false \
    --set api=chat-completions --set mcp/enabled=true --set tools=true \
    --set "mcp/servers/src={transport: stdio, command: '$PYTHON', args: ['$TESTS_DIR/mock/mock_mcp_events.py'], env: {MCP_EVENTS_LOG: '$EVENTS_LOG', MCP_EVENTS_FIFO: '$TEST_DIR/events.fifo', MCP_EVENTS_CRASHABLE: '1'}, cwd: '$TEST_DIR', events: true}" \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: restart the source
  - wait: Restarted.
    timeout: session
  - type: /exit
PTY
grep -a -q "source 'src' went down; it starts again" "$TEST_DIR/pty.out" ||
	fail "the restart was not announced"
grep -a -q "source 'src' is up again" "$TEST_DIR/pty.out" ||
	fail "the source did not say it is up again"
mock_stop 4

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the events around the restart"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
def tool(req, call):
    for m in req["body"]["messages"]:
        if m.get("role") == "tool" and m.get("tool_call_id") == call:
            return m["content"]
    raise SystemExit("no result for %s" % call)
assert '"first"' in tool(reqs[1], "call_wait1"), tool(reqs[1], "call_wait1")
second = tool(reqs[3], "call_wait2")
assert second.startswith("[event 'src/note'] fifo-"), second
assert '"second"' in second, second
# The tool of the started process is offered again.
assert any(t["function"]["name"] == "mcp__src__crash"
           for t in reqs[3]["body"]["tools"]), "the tools did not come back"
PY

"$PYTHON" - "$EVENTS_LOG" <<'PY' || fail "the source did not start again"
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
inits = [i for i, r in enumerate(rows) if r.get("method") == "initialize"]
crash = [i for i, r in enumerate(rows) if "crash" in r]
assert len(inits) == 2 and len(crash) == 1, rows
assert inits[0] < crash[0] < inits[1], rows
PY
pass
