#!/bin/bash
# SPDX-License-Identifier: MIT
# The preview of a session renders with the display settings of its branch,
# not with those of the session that browses it. A branch that stores the
# Ember palette theme shows the Ember user card in the resume picker of a
# session that uses the plain dark theme.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_themed_markdown.json

# The newest session stores its own theme.
run_fyai -b palbranch -k test-key --set display/markdown=true \
	--set display/theme=ember:dark --set display/stream=false \
	--set api=chat-completions \
	--set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status 0
mock_stop_quiet

COLORTERM=truecolor \
FYAI_PTY_ROWS=40 FYAI_PTY_COLS=120 \
FYAI_PTY_INPUT="" FYAI_PTY_SUBMIT_INPUT=0 \
FYAI_PTY_READY_NEEDLE="toggle foreign sessions" \
FYAI_PTY_NEEDLE="toggle foreign sessions" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="raw:1b5b42|wait-screen:Heading|raw:1b" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pick.out" \
	"$FYAI_BIN" -k test-key --color on --theme dark -m mock-model \
	--set display/markdown=true --set display/branch_preview=bottom resume

if grep -a -q "invalid display.theme 'ember:dark'" "$TEST_DIR/pick.out"; then
	skip "fyai is built without libfypalette"
fi
grep -a -q "toggle foreign sessions" "$TEST_DIR/pick.out" ||
	fail "the picker did not open"
# The raised card ground of Ember dark.
grep -a -q $'\x1b\\[48;2;24;22;18m' "$TEST_DIR/pick.out" ||
	fail "the preview did not use the display settings of its branch"

pass
