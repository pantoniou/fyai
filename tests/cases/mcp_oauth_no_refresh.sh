#!/bin/bash
# SPDX-License-Identifier: MIT
# Accept a successful OAuth response that does not include a refresh token.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_oauth_no_refresh.json
export PATH="$TESTS_DIR/mock:$PATH"

run_fyai --set api=chat-completions --set display/stream=false \
	--set mcp/enabled=true \
	--set "mcp/servers/default={endpoint: '$MOCK_URL/mcp', auth: \
{type: oauth, allow_browser: true}}" \
	--set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "connect MCP"
assert_status 0
assert_stdout_contains "OAuth connected without refresh."
assert_any_request \
	'r["path"] == "/mcp" and r["auth"] == "Bearer access-only-token"'

mock_stop 9
pass
