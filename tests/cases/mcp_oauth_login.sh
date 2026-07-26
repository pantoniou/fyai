#!/bin/bash
# SPDX-License-Identifier: MIT
# Complete MCP OAuth browser callback and retry the parked initialization.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_oauth_login.json
export PATH="$TESTS_DIR/mock:$PATH"

run_fyai --set api=chat-completions --set display/stream=false \
	--set mcp/enabled=true \
	--set "mcp/servers/default={endpoint: '$MOCK_URL/mcp', auth: \
{type: oauth, allow_browser: true}}" \
	--set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "connect MCP"
assert_status 0
assert_stdout_contains "OAuth connected."
assert_any_request \
	'r["path"] == "/oauth/token" and "code=mcp-test-code" in r["body"]'
assert_any_request \
	'r["path"] == "/mcp" and r["auth"] == "Bearer mcp-access-token"'
assert_any_request \
	'r["path"] == "/oauth/token" and "grant_type=refresh_token" in r["body"]'

mock_stop 10
pass
