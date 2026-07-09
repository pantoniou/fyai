#!/bin/bash
# SPDX-License-Identifier: MIT
# Theming: stylistic options live in the display: group; markdown_theme
# selects a shipped styling by name and the renderer accepts it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json

cat > themed.yaml << 'EOF'
display:
  markdown: true
  markdown_mode: oneshot
  theme: dark
  markdown_theme: vivid
  code_theme: kanagawa
EOF
run_fyai config import themed.yaml
assert_status 0

# nested keys land in the effective config
run_fyai config effective
assert_status 0
assert_stdout_contains "markdown_theme: vivid"
assert_stdout_contains "code_theme: kanagawa"
assert_stdout_contains "markdown_mode: oneshot"

# a themed markdown run works end to end (markdown forced on over the
# harness default; --color on exercises the styling load path)
"$FYAI_BIN" -k test-key --color on --markdown --markdown-mode oneshot \
	--set api=chat-completions --no-stream -u "$MOCK_URL/v1/chat/completions" \
	-m mock-model "hello" >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
FYAI_STATUS=$?
assert_status 0
assert_stdout_contains "Hello from the mock"

mock_stop_quiet

# an unknown theme falls back to the default with a warning, not a failure
# (the styling loads only when markdown rendering is actually on)
mock_start chat_basic.json
"$FYAI_BIN" -k test-key --color on --markdown --markdown-mode oneshot \
	--markdown-theme no-such-theme --new \
	--set api=chat-completions --no-stream -u "$MOCK_URL/v1/chat/completions" \
	-m mock-model "hello" >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
FYAI_STATUS=$?
assert_status 0
assert_stdout_contains "Hello from the mock"
assert_stderr_contains "markdown theme 'no-such-theme' not found"

mock_stop_quiet
pass
