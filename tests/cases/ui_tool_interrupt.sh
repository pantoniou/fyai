#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_tool_interrupt.json

start=$(date +%s)
FYAI_PTY_INPUT="run it" \
FYAI_PTY_PROGRESS_NEEDLE="interrupt-early" \
FYAI_PTY_PROGRESS_TIMEOUT="2" \
FYAI_PTY_INTERRUPT_AFTER_PROGRESS="1" \
FYAI_PTY_NEEDLE="interrupted" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set tools=true --set api=chat-completions \
	--set "api_url=$MOCK_URL/v1/chat/completions" \
	-m mock-model -i
elapsed=$(($(date +%s) - start))

[ "$elapsed" -lt 8 ] || fail "ESC did not interrupt the active tool"
[ ! -f interrupt-late.marker ] || fail "tool continued after ESC"

mock_stop 1
pass
