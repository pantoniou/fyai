#!/bin/bash
# SPDX-License-Identifier: MIT
# The Markdown theme controls its palette, background, fenced code, and chrome.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_themed_markdown.json

cat > themed.yaml << 'EOF'
display:
  markdown: true
  markdown_mode: oneshot
  theme: catppuccin:dark
EOF
run_fyai config import themed.yaml
assert_status 0

# nested keys land in the effective config
run_fyai config effective
assert_status 0
assert_stdout_contains "theme: catppuccin:dark"
assert_stdout_contains "markdown_mode: oneshot"

# a themed markdown run works end to end (markdown forced on over the
# harness default; --color on exercises the styling load path)
set +e
"$FYAI_BIN" -k test-key --color on --set display/markdown=true --set display/markdown_mode=oneshot \
	--set api=chat-completions --set display/stream=false --set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "hello" >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
FYAI_STATUS=$?
set -e
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'EOF' || \
	fail "Markdown theme did not reach both Markdown and fenced-code renderers"
import sys

data = open(sys.argv[1], "rb").read()
if b"\x1b[1;38;2;203;166;247mHeading" not in data:
    raise SystemExit("catppuccin Markdown heading colour missing")
if b"\x1b[38;2;249;226;175mint" not in data:
    raise SystemExit("catppuccin libfyts keyword colour missing")
EOF

mock_stop_quiet

# The interactive selector is a durable preference, not merely a restyle of
# the current process.
"$FYAI_BIN" -k test-key --color off -m mock-model -i \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" <<'EOF'
/theme solarized:light
/exit
EOF
run_fyai config get display/theme
assert_status 0
assert_stdout_contains "solarized:light"

# an unknown theme is rejected at config validation (no libfymd4c theme by
# that name), rather than silently falling back
run_fyai --set display/theme=no-such-theme:dark --new -m mock-model "hello"
assert_status 1
assert_stderr_contains "invalid display.theme 'no-such-theme:dark'"

pass
