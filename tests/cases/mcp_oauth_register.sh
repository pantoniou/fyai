#!/bin/bash
# SPDX-License-Identifier: MIT
# MCP OAuth discovery and dynamic client registration stay on the event loop.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_oauth_register.json

run_fyai --set api=chat-completions --set display/stream=false \
	--set mcp/enabled=true \
	--set "mcp/servers/default={endpoint: '$MOCK_URL/mcp', auth: {type: oauth}}" \
	--set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "try MCP"
assert_status 0
assert_stdout_contains "OAuth unavailable."
assert_stderr_contains "browser login needs auth.allow_browser=true"

assert_request 0 'r["path"] == "/mcp"'
assert_request 1 'r["path"] == "/oauth/resource"'
assert_request 2 \
	'r["path"] == "/.well-known/oauth-authorization-server/issuer"'
assert_request 3 \
	'r["path"] == "/oauth/register" and r["body"]["client_name"] == "fyai"'
assert_request 3 \
	'r["body"]["token_endpoint_auth_method"] == "none"'
assert_request 3 \
	'len(r["body"]["redirect_uris"]) == 1 and '\
'r["body"]["redirect_uris"][0].startswith("http://127.0.0.1:") and '\
'r["body"]["redirect_uris"][0].endswith("/mcp/oauth/callback")'
if ! test -f "$XDG_STATE_HOME/fyai/mcp-auth-default.json"; then
	fail "MCP OAuth client was not stored"
fi
if ! "$PYTHON" - "$XDG_STATE_HOME/fyai/mcp-auth-default.json" <<'EOF'
import json, os, stat, sys
path = sys.argv[1]
mode = stat.S_IMODE(os.stat(path).st_mode)
data = json.load(open(path))
sys.exit(0 if mode == 0o600 and
         data.get("client_id") == "fyai-test-client" else 1)
EOF
then
	fail "MCP OAuth store is invalid or not private"
fi

mock_stop 5
pass
