#!/bin/bash
# SPDX-License-Identifier: MIT
# The browser renders the end of the selected session beside the branch view.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_basic.json

FYAI_PTY_COLS=140 \
FYAI_PTY_ROWS=40 \
FYAI_PTY_INPUT="hello" \
FYAI_PTY_NEEDLE="Hello from the mock responses provider." \
FYAI_PTY_AFTER="send:/branches|wait:session end|raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/preview.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set api=responses --set display/branch_preview=right \
	--set display/branch_preview_width=48 \
	--set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

mock_stop 1
pass
