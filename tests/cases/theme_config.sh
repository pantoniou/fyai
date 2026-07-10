#!/bin/bash
# SPDX-License-Identifier: MIT
# Theming: stylistic options live in the display: group; markdown_theme
# selects a libfymd4c embedded theme by name and the renderer accepts it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json

cat > themed.yaml << 'EOF'
display:
  markdown: true
  markdown_mode: oneshot
  theme: dark
  markdown_theme: catppuccin
  code_theme: kanagawa
EOF
run_fyai config import themed.yaml
assert_status 0

# nested keys land in the effective config
run_fyai config effective
assert_status 0
assert_stdout_contains "markdown_theme: catppuccin"
assert_stdout_contains "code_theme: kanagawa"
assert_stdout_contains "markdown_mode: oneshot"

# a themed markdown run works end to end (markdown forced on over the
# harness default; --color on exercises the styling load path)
"$FYAI_BIN" -k test-key --color on --set display/markdown=true --set display/markdown_mode=oneshot \
	--set api=chat-completions --set display/stream=false -u "$MOCK_URL/v1/chat/completions" \
	-m mock-model "hello" >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
FYAI_STATUS=$?
assert_status 0
assert_stdout_contains "Hello from the mock"

mock_stop_quiet

# an unknown theme is rejected at config validation (no libfymd4c theme by
# that name), rather than silently falling back
run_fyai --set display/markdown_theme=no-such-theme --new -m mock-model "hello"
assert_status 1
assert_stderr_contains "invalid display.markdown_theme 'no-such-theme'"

pass
