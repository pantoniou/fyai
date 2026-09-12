#!/bin/bash
# SPDX-License-Identifier: MIT
# A palette theme styles the Markdown, the fenced code and the chrome from one
# set of colours, for the dark and the light variant.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

run_themed() {
	set +e
	"$FYAI_BIN" -k test-key --color on --set display/markdown=true \
		--set display/markdown_mode=oneshot --set display/theme="$1" \
		--set api=chat-completions --set display/stream=false \
		--set api_url="$MOCK_URL/v1/chat/completions" \
		-m mock-model "hello" >"$TEST_DIR/$2" 2>"$TEST_DIR/stderr"
	FYAI_STATUS=$?
	set -e
}

mock_start chat_themed_markdown.json
run_themed ember:dark dark.out
mock_stop_quiet
if grep -q "invalid display.theme 'ember:dark'" "$TEST_DIR/stderr"; then
	skip "fyai is built without libfypalette"
fi
assert_status 0

mock_start chat_themed_markdown.json
run_themed ember:light light.out
mock_stop_quiet
assert_status 0

"$PYTHON" - "$TEST_DIR/dark.out" "$TEST_DIR/light.out" <<'EOF' || \
	fail "the palette theme did not reach the Markdown and fenced-code renderers"
import re
import sys

def heading(path):
    data = open(path, "rb").read()
    m = re.search(rb"\x1b\[1;38;2;(\d+);(\d+);(\d+)mHeading", data)
    if not m:
        raise SystemExit(f"{path}: no palette heading colour")
    if not re.search(rb"\x1b\[[0-9;]*38;2;\d+;\d+;\d+mint\b", data):
        raise SystemExit(f"{path}: no palette colour on the fenced code")
    return tuple(int(v) for v in m.groups())

dark = heading(sys.argv[1])
light = heading(sys.argv[2])
# Ember ink is warm: red over green over blue, light on dark, dark on light.
if not (dark[0] >= dark[1] >= dark[2] and min(dark) > 160):
    raise SystemExit(f"dark ink {dark} is not a light warm neutral")
if not (light[0] >= light[1] >= light[2] and max(light) < 90):
    raise SystemExit(f"light ink {light} is not a dark warm neutral")
EOF

# the palette themes are offered next to the Markdown themes
run_fyai --set display/theme=no-such-theme:dark --new -m mock-model "hello"
assert_status 1
assert_stderr_contains "ember"

pass
