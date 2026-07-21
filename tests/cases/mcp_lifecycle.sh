#!/bin/bash
# SPDX-License-Identifier: MIT
# Synchronous MCP retry, pagination, session recovery and shutdown.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_lifecycle.json

run_fyai --set api=chat-completions --set display/stream=false \
	--set mcp/enabled=true --set "mcp/endpoint='$MOCK_URL/mcp'" \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "use the second MCP tool"
assert_status 0
assert_stdout_contains "MCP lifecycle completed."

assert_request 0 'r["body"]["method"] == "initialize" and r["body"]["id"] == 1'
assert_request 1 'r["body"]["method"] == "initialize" and r["body"]["id"] == 1'
assert_request 3 'r["body"] == {"jsonrpc":"2.0","method":"tools/list","params":{},"id":2}'
assert_request 4 'r["body"]["method"] == "tools/list" and r["body"]["params"] == {"cursor":"next-page"}'
assert_request 5 'any(t["function"]["name"] == "mcp__default__first" for t in r["body"]["tools"])'
assert_request 5 'any(t["function"]["name"] == "mcp__default__second" for t in r["body"]["tools"])'
assert_request 6 'r["body"]["method"] == "tools/call" and r["body"]["id"] == 4 and r["mcp_session_id"] == "old-session"'
assert_request 7 'r["body"]["method"] == "initialize" and r["body"]["id"] == 5 and not r["mcp_session_id"]'
assert_request 9 'r["body"]["method"] == "tools/call" and r["body"]["id"] == 4 and r["mcp_session_id"] == "new-session"'
assert_request 11 'r["method"] == "DELETE" and r["mcp_session_id"] == "new-session"'

mock_stop 12
pass
