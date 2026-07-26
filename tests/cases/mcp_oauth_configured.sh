#!/bin/bash
# SPDX-License-Identifier: MIT
# A configured OAuth client bypasses dynamic client registration.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_oauth_configured.json
export PATH="$TESTS_DIR/mock:$PATH"
export GOOGLE_GMAIL_MCP_CLIENT_SECRET=mcp-import-secret-value
export FYAI_BROWSER_URL_LOG="$TEST_DIR/browser-url"

run_fyai mcp oauth import-client gmail \
	"$TESTS_DIR/data/google_oauth_client.json" \
	--endpoint "$MOCK_URL/mcp" \
	--scope https://www.googleapis.com/auth/gmail.readonly \
	--secret-env GOOGLE_GMAIL_MCP_CLIENT_SECRET
assert_status 0
run_fyai config set mcp/servers/gmail/auth/allow_browser true
assert_status 0
run_fyai config set mcp/enabled true
assert_status 0

run_fyai --set api=chat-completions --set display/stream=false \
	--set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "connect MCP"
assert_status 0
assert_stdout_contains "Configured OAuth connected."
token_request='r["path"] == "/oauth/token"'
assert_any_request \
	"$token_request and "\
'"client_secret=mcp-import-secret-value" in r["body"]'
assert_any_request \
	'r["path"] == "/oauth/token" and "code=mcp-test-code" in r["body"]'
assert_any_request \
	"$token_request and \"resource=\" in r[\"body\"]"
assert_any_request \
	'r["path"] == "/mcp" and r["auth"] == "Bearer configured-access-token"'
assert_any_request \
	"$token_request and \"grant_type=refresh_token\" in r[\"body\"] "\
"and \"resource=\" in r[\"body\"]"
assert_any_request \
'isinstance(r["body"], dict) and '\
'r["body"].get("method") == "tools/call" and '\
'r["auth"] == "Bearer configured-refreshed-token"'

"$PYTHON" - "$FYAI_BROWSER_URL_LOG" "$MOCK_URL/mcp" <<'PY'
import sys
import urllib.parse

url = open(sys.argv[1], encoding="utf-8").read().strip()
query = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
assert query["scope"] == [
    "https://www.googleapis.com/auth/gmail.readonly"
]
assert query["access_type"] == ["offline"]
assert query["prompt"] == ["consent"]
assert query["resource"] == [sys.argv[2]]
assert query["redirect_uri"][0].startswith("http://localhost:")
PY

mock_stop 14
pass
