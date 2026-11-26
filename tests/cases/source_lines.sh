#!/bin/bash
# SPDX-License-Identifier: MIT
# `fyai source lines` as an MCP event source: a line of a file that passes
# the schema reaches the model and starts a turn, a bad line is skipped, the
# acknowledged offset is kept in the cursor file, and the next run resumes
# after it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start source_lines.json
cat >"$TEST_DIR/schema.json" <<'JSON'
{"type": "object", "required": ["text"], "properties": {"text": {"type": "string"}}}
JSON
printf '%s\n' '{"text": 42}' '{"text": "first message"}' >"$TEST_DIR/inbox.jsonl"

session() {
	pty_run "$TEST_DIR/$1.out" \
	    "$FYAI_BIN" -k test-key --set display/stream=false \
	    --set api=chat-completions --set mcp/enabled=true \
	    --set "mcp/servers/inbox={transport: stdio, command: '$FYAI_BIN', args: [source, lines, --kind, message, --json, --schema, '$TEST_DIR/schema.json', --cursor-file, '$TEST_DIR/inbox.cursor', --, cat, '$TEST_DIR/inbox.jsonl'], cwd: '$TEST_DIR', events: true}" \
	    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<PTY
timeout: 30
steps:
  - wait: $2
    timeout: session
  - type: /exit
PTY
}

# The first run: the good line starts a turn by itself.
session run1 "First seen."
[ "$(cat "$TEST_DIR/inbox.cursor")" = "$(wc -c <"$TEST_DIR/inbox.jsonl" | tr -d " ")" ] ||
	fail "the cursor is not the end of the acknowledged line"

# The second run skips what the first acknowledged.
printf '%s\n' '{"text": "second message"}' >>"$TEST_DIR/inbox.jsonl"
session run2 "Second seen."
[ "$(cat "$TEST_DIR/inbox.cursor")" = "$(wc -c <"$TEST_DIR/inbox.jsonl" | tr -d " ")" ] ||
	fail "the second run did not move the cursor"

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the events of the lines"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
assert len(reqs) == 2, len(reqs)
def users(r):
    return "\n".join(m["content"] for m in r["body"]["messages"]
                     if m.get("role") == "user" and isinstance(m.get("content"), str))
first, second = users(reqs[0]), users(reqs[1])
assert "[event 'inbox/message'] line-13" in first, first
assert '"first message"' in first, first
assert "42" not in first, first
assert "[event 'inbox/message'] line-39" in second, second
assert "first message" not in second, second
PY

mock_stop 2
pass
