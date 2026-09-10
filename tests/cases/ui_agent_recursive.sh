#!/bin/bash
# SPDX-License-Identifier: MIT
# Grandchildren have compact progress in their parent's terminal.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_recursive_progress.json

FYAI_PTY_INPUT="delegate recursively" \
FYAI_PTY_MID_NEEDLE="Agent progress" \
FYAI_PTY_MID_TIMEOUT=8 \
FYAI_PTY_NEEDLE="Recursive delegation complete." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set tools=true --set api=responses \
	--set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

mock_stop 5
pass
