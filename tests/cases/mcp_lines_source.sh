#!/bin/bash
# SPDX-License-Identifier: MIT
# A line source in the configuration: `lines: COMMAND` runs the command
# through `fyai source lines` and sends each line as an event of its kind,
# here JSON lines checked against an inline schema.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_lines_source.json
mkfifo "$TEST_DIR/events.fifo"

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --set display/stream=false \
    --set api=chat-completions --set mcp/enabled=true \
    --set "mcp/servers/logs={lines: 'cat events.fifo', kind: entry, json: true, schema: {type: object, required: [n], properties: {n: {type: integer}}}, cwd: '$TEST_DIR'}" \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: wait for the lines
  - wait: Lines seen.
    timeout: session
  - type: /exit
PTY
mock_stop 2

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the line did not reach the model as an event"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
for m in reqs[1]["body"]["messages"]:
    if m.get("role") == "tool" and m.get("tool_call_id") == "call_wait":
        waited = m["content"]
        break
else:
    raise SystemExit("no result of the wait")
assert waited.startswith("[event 'logs/entry']"), waited
assert '"n":1' in waited.replace(" ", ""), waited
# The line that fails the schema is not an event.
assert "bad" not in waited, waited
PY
pass
