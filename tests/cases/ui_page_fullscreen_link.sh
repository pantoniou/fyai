#!/bin/bash
# SPDX-License-Identifier: MIT
# A link in the prose of a fullscreen answer reaches the terminal as an OSC 8
# link, with its whole URI: the page draws the transcript as cells, and a cell
# keeps the link it was drawn with.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_fullscreen_link.json
FYAI_PTY_ROWS=20 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="give me a link" \
FYAI_PTY_NEEDLE="LINK-END" \
FYAI_PTY_AFTER="wait-screen:LINK-END" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/renderer=page --set display/screen=fullscreen \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR/scenarios/chat_fullscreen_link.json" <<'PY' ||
import json
import re
import sys

data = open(sys.argv[1], "rb").read()
answer = json.load(open(sys.argv[2]))["steps"][0]["response"]["choices"][0]
long_uri = re.search(r"\]\(([^)]+)\)", answer["message"]["content"]).group(1)
for uri in (long_uri, "https://example.com/bare/path"):
    if b"\x1b]8;;" + uri.encode() + b"\x1b\\" not in data:
        raise SystemExit("no OSC 8 link to %s..." % uri[:48])
PY
    fail "a link of the answer is not a link on the screen"

mock_stop 1
pass
