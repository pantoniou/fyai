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
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "use the beta MCP tool"
assert_status 0
assert_stdout_contains "Multiple MCP servers work."

# Servers initialize concurrently, so requests interleave: assert by content,
# not by position. Each server initializes (alpha unauthenticated, beta with its
# token) and lists tools; both tools reach the model catalogue.
assert_any_request 'r["body"] and r["body"].get("method") == "initialize" and not r["auth"]'
assert_any_request 'r["body"] and r["body"].get("method") == "initialize" and r["auth"] == "Bearer mcp-beta-secret"'
assert_any_request 'r["body"] and r["body"].get("method") == "tools/list" and not r["auth"]'
assert_any_request 'r["body"] and r["body"].get("method") == "tools/list" and r["auth"] == "Bearer mcp-beta-secret"'
assert_any_request 'isinstance(r["body"], dict) and "tools" in r["body"] and any(t["function"]["name"] == "mcp__alpha__echo" for t in r["body"]["tools"]) and any(t["function"]["name"] == "mcp__beta__echo" for t in r["body"]["tools"])'
# The model's beta tool call is routed to beta with its token.
assert_any_request 'r["body"] and r["body"].get("method") == "tools/call" and r["body"]["params"]["name"] == "echo" and r["auth"] == "Bearer mcp-beta-secret"'
assert_any_request 'isinstance(r["body"], dict) and any(m.get("role") == "tool" and m.get("content") == "beta: hello" for m in r["body"].get("messages", []))'
# Both sessions are deleted on shutdown.
assert_any_request 'r.get("method") == "DELETE" and r["mcp_session_id"] == "alpha-session"'
assert_any_request 'r.get("method") == "DELETE" and r["mcp_session_id"] == "beta-session"'

mock_stop 11
pass
