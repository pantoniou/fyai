#!/bin/bash
# SPDX-License-Identifier: MIT
# The MCP servers of fyai serve the Claude Code CLI too: their tools are in
# what the CLI is offered, and a call of one goes to its server, with its
# token, and the result goes back to the CLI.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_multiple.json
export MCP_BETA_TOKEN=mcp-beta-secret
fake_claude_install claude_code_mcp.json

run_fyai --set api=claude-code --set "claude_code/command=$FAKE_CLAUDE" \
	--set mcp/enabled=true \
	--set "mcp/servers={alpha: {endpoint: '$MOCK_URL/mcp'}, beta: {endpoint: '$MOCK_URL/mcp', auth_token: {type: env, value: MCP_BETA_TOKEN}}}" \
	"use the beta MCP tool"
assert_status 0
assert_stdout_contains "MCP works through the CLI."

"$PYTHON" - "$FAKE_CLAUDE_LOG" <<'PYEOF' || fail "the CLI and the MCP tools"
import json, sys
log = [json.loads(l) for l in open(sys.argv[1])]
names = [e for e in log if e["event"] == "tools"][0]["names"]
assert "mcp__alpha__echo" in names and "mcp__beta__echo" in names, names
calls = [e for e in log if e["event"] == "call"]
assert len(calls) == 1 and calls[0]["id"] == "toolu_mcp", calls
assert calls[0]["result"]["content"][0]["text"] == "beta: hello", calls
PYEOF
assert_any_request 'r["body"] and r["body"].get("method") == "tools/call" and r["body"]["params"]["name"] == "echo" and r["body"]["params"]["arguments"] == {"text": "hello"} and r["auth"] == "Bearer mcp-beta-secret"'
mock_stop
pass
