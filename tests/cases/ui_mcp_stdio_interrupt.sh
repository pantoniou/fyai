#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_stdio.json
STDIO_LOG="$TEST_DIR/mcp-stdio.jsonl"

start=$(date +%s)
# Gate the interrupt on the call reaching the mock: the PTY shows the
# turn is busy while the MCP handshake still runs, and an interrupt in
# that window recalls nothing. The mock log records tools/call once the
# call the interrupt must land in is truly in flight.
pty_run "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set tools=true --set api=chat-completions \
	--set mcp/enabled=true \
	--set "mcp/servers/local={transport: stdio, command: '$PYTHON', args: \
['$TESTS_DIR/mock/mock_mcp_stdio.py'], env: {MCP_STDIO_LOG: '$STDIO_LOG', \
MCP_CALL_DELAY: '60'}, cwd: '$TEST_DIR'}" \
	--set "api_url=$MOCK_URL/v1/chat/completions" \
	-m mock-model -i <<'PTY'
exit: {clear: true}
steps:
  - type: use local echo
  - wait-file: "${TEST_DIR}/served"
    equals: "1"
  - write: "pending input\n"
  - wait: pending input
    timeout: 10
  - wait-file: "${TEST_DIR}/mcp-stdio.jsonl"
    contains: tools/call
    timeout: 10
  - write: "\e"
  - wait-count: pending input
    more: 1
    timeout: 10
  - wait: interrupted
    timeout: session
PTY
elapsed=$(($(date +%s) - start))

[ "$elapsed" -lt $((30 * FYAI_TIMEOUT_SCALE)) ] || \
	fail "ESC did not interrupt the MCP pipe request"

mock_stop 1

pty_run "$TEST_DIR/mcp-status.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set mcp/enabled=true \
	--set "mcp/servers/local={transport: stdio, command: '$PYTHON', args: \
['$TESTS_DIR/mock/mock_mcp_stdio.py'], cwd: '$TEST_DIR'}" \
	-m mock-model -i <<'PTY'
steps:
  - type: "/mcp"
  - wait: MCP endpoints
    timeout: session
PTY

grep -qF "MCP endpoints" "$TEST_DIR/mcp-status.out" || \
	fail "/mcp did not render the endpoint status table"

pass
