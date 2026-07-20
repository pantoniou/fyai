#!/bin/bash
# SPDX-License-Identifier: MIT
# MCP Streamable HTTP discovery and tool-call round trip.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp.json
export MCP_TEST_TOKEN=mcp-secret-for-test

run_fyai --set api=chat-completions --set display/stream=false \
	--set logging/mcp=true \
	--set mcp/enabled=true --set "mcp/endpoint='$MOCK_URL/mcp'" \
	--set 'mcp/auth_token={type: env, value: MCP_TEST_TOKEN}' \
	-u "$MOCK_URL/v1/chat/completions" -m mock-model "use the MCP echo tool"
assert_status 0
assert_stdout_contains "MCP call completed."

assert_request 0 'r["path"] == "/mcp" and r["body"]["method"] == "initialize"'
assert_request 0 'r["auth"] == "Bearer mcp-secret-for-test"'
assert_request 1 'r["path"] == "/mcp" and r["body"]["method"] == "notifications/initialized"'
assert_request 1 'r["mcp_session_id"] == "test-session"'
assert_request 2 'r["path"] == "/mcp" and r["body"]["method"] == "tools/list" and r["mcp_session_id"] == "test-session"'
assert_request 2 'r["mcp_protocol_version"] == "2024-11-05"'
assert_request 3 'any(t["function"]["name"] == "mcp__default__echo" for t in r["body"]["tools"])'
assert_request 3 'all(t["function"]["name"].startswith("mcp__") for t in r["body"]["tools"])'
assert_request 4 'r["path"] == "/mcp" and r["body"]["method"] == "tools/call" and r["body"]["params"] == {"name":"echo","arguments":{"text":"hello"}}'
assert_request 4 'len({reqs[i]["client_port"] for i in (0,1,2,4)}) == 1'
assert_request 5 'any(m.get("role") == "tool" and m.get("content") == "echo: hello" for m in r["body"]["messages"])'

mock_stop 6
test -s .fyai/logs/mcp.yaml || fail "missing MCP log"
grep -q 'event: discovery' .fyai/logs/mcp.yaml || fail "MCP log missing discovery"
grep -q 'event: tool_call' .fyai/logs/mcp.yaml || fail "MCP log missing tool call"
grep -q 'elapsed_ms:' .fyai/logs/mcp.yaml || fail "MCP log missing timing"
! grep -q 'mcp-secret-for-test' .fyai/logs/mcp.yaml || fail "MCP log leaked auth token"
pass
