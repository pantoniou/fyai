#!/bin/bash
# SPDX-License-Identifier: MIT
# Multiple named MCP servers retain independent sessions and route tools.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_multiple.json
export MCP_BETA_TOKEN=mcp-beta-secret

run_fyai --set api=chat-completions --set display/stream=false \
	--set mcp/enabled=true \
	--set "mcp/servers={alpha: {endpoint: '$MOCK_URL/mcp'}, beta: {endpoint: '$MOCK_URL/mcp', auth_token: {type: env, value: MCP_BETA_TOKEN}}}" \
	-u "$MOCK_URL/v1/chat/completions" -m mock-model "use the beta MCP tool"
assert_status 0
assert_stdout_contains "Multiple MCP servers work."

assert_request 0 'r["body"]["method"] == "initialize" and not r["auth"]'
assert_request 1 'r["body"]["method"] == "notifications/initialized"'
assert_request 2 'r["body"]["method"] == "tools/list"'
assert_request 3 'r["body"]["method"] == "initialize" and r["auth"] == "Bearer mcp-beta-secret"'
assert_request 4 'r["body"]["method"] == "notifications/initialized"'
assert_request 5 'r["body"]["method"] == "tools/list"'
assert_request 6 'any(t["function"]["name"] == "mcp__alpha__echo" for t in r["body"]["tools"])'
assert_request 6 'any(t["function"]["name"] == "mcp__beta__echo" for t in r["body"]["tools"])'
assert_request 7 'r["body"]["method"] == "tools/call" and r["body"]["params"]["name"] == "echo"'
assert_request 7 'r["auth"] == "Bearer mcp-beta-secret"'
assert_request 7 'len({reqs[i]["client_port"] for i in (3,4,5,7)}) == 1'
assert_request 8 'any(m.get("role") == "tool" and m.get("content") == "beta: hello" for m in r["body"]["messages"])'
assert_request 9 'r["method"] == "DELETE" and r["mcp_session_id"] == "alpha-session"'
assert_request 10 'r["method"] == "DELETE" and r["mcp_session_id"] == "beta-session"'

mock_stop 11
pass
