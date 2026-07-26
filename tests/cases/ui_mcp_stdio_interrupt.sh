#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_stdio.json
STDIO_LOG="$TEST_DIR/mcp-stdio.jsonl"

start=$(date +%s)
FYAI_PTY_INPUT="use local echo" \
FYAI_PTY_DURING_INPUT="pending input" \
FYAI_PTY_INTERRUPT_AFTER_DURING="1" \
FYAI_PTY_NEEDLE="interrupted" \
FYAI_PTY_CLEAR_BEFORE_EXIT="1" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set tools=true --set api=chat-completions \
	--set mcp/enabled=true \
	--set "mcp/servers/local={transport: stdio, command: '$PYTHON', args: \
['$TESTS_DIR/mock/mock_mcp_stdio.py'], env: {MCP_STDIO_LOG: '$STDIO_LOG', \
MCP_CALL_DELAY: '5'}, cwd: '$TEST_DIR'}" \
	--set "api_url=$MOCK_URL/v1/chat/completions" \
	-m mock-model -i
elapsed=$(($(date +%s) - start))

[ "$elapsed" -lt 8 ] || fail "ESC did not interrupt the MCP pipe request"

mock_stop 1
pass
