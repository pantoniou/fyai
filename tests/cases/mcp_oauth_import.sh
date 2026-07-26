#!/bin/bash
# SPDX-License-Identifier: MIT
# Importing a Google OAuth client retains only a secret reference.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

run_fyai mcp oauth import-client gmail \
	"$TESTS_DIR/data/google_oauth_client.json" \
	--endpoint https://gmailmcp.googleapis.com/mcp/v1 \
	--scope https://www.googleapis.com/auth/gmail.readonly \
	--secret-env GOOGLE_GMAIL_MCP_CLIENT_SECRET
assert_status 0
assert_stdout_contains "imported OAuth client for gmail"
assert_stdout_not_contains "mcp-import-secret-value"

run_fyai config get mcp/servers/gmail
assert_status 0
assert_stdout_contains "test-client.apps.googleusercontent.com"
assert_stdout_contains "GOOGLE_GMAIL_MCP_CLIENT_SECRET"
assert_stdout_contains "gmail.readonly"
assert_stdout_not_contains "mcp-import-secret-value"

run_fyai mcp oauth import-client gmail \
	"$TESTS_DIR/data/google_oauth_client.json" \
	--endpoint https://gmailmcp.googleapis.com/mcp/v1 \
	--scope https://www.googleapis.com/auth/gmail.readonly \
	--secret-env GOOGLE_GMAIL_MCP_CLIENT_SECRET
assert_status_nonzero
assert_stderr_contains "already exists"

run_fyai mcp oauth import-client gmail \
	"$TESTS_DIR/data/google_oauth_client.json" \
	--endpoint https://gmailmcp.googleapis.com/mcp/v1 \
	--scope https://www.googleapis.com/auth/gmail.readonly \
	--secret-env GOOGLE_GMAIL_MCP_CLIENT_SECRET --force
assert_status 0

pass
