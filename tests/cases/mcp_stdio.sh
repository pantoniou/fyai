#!/bin/bash
# SPDX-License-Identifier: MIT
# Persistent local MCP stdio server discovery, call and EOF cleanup.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_stdio.json
STDIO_LOG="$TEST_DIR/mcp-stdio.jsonl"

run_fyai --set api=chat-completions --set display/stream=false \
	--set mcp/enabled=true \
	--set "mcp/servers/local={transport: stdio, command: '$PYTHON', args: ['$TESTS_DIR/mock/mock_mcp_stdio.py'], env: {MCP_STDIO_LOG: '$STDIO_LOG', MCP_TEST_VALUE: configured}, cwd: '$TEST_DIR'}" \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "use local echo"
assert_status 0
assert_stdout_contains "Stdio MCP completed."

"$PYTHON" - "$STDIO_LOG" "$TEST_DIR" <<'PY'
import json, os, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert [r["request"]["method"] for r in rows[:-1]] == [
    "initialize", "notifications/initialized", "tools/list", "tools/call"]
# compare resolved paths: on macOS the sandbox lives under /var/folders, which
# is a symlink to /private/var/folders, so the child reports the resolved form
assert os.path.realpath(rows[0]["cwd"]) == os.path.realpath(sys.argv[2])
assert rows[0]["env"] == "configured"
assert rows[-1] == {"eof": True}
PY
assert_request 0 'any(t["function"]["name"] == "mcp__local__echo" for t in r["body"]["tools"])'
assert_request 1 'any(m.get("role") == "tool" and m.get("content") == "stdio: hello" for m in r["body"]["messages"])'

mock_stop 2
pass
