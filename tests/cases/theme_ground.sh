#!/bin/bash
# SPDX-License-Identifier: MIT
# display/theme_ground=terminal makes the background that the terminal reports
# the ground of a palette theme. Over a blue-black terminal, the raised ground
# of Ember - the card and the fenced block - then takes the hue of that
# background; with the ground of the theme it stays warm. The ink steps up
# past white over that ground, so the ink cannot show the hue.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

session()
{
    fyai_test_setup
    mock_start chat_themed_markdown.json
    driver=0
    # The grounds are compared as RGB, which a 256-colour terminal cannot draw.
    COLORTERM=truecolor FYAI_PTY_BACKGROUND=rgb:1e1e/1e1e/2e2e \
    FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="hello" \
    FYAI_PTY_NEEDLE="Heading" FYAI_PTY_TIMEOUT=20 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --color on --set display/markdown=true \
        --set display/theme=ember:dark --set "display/theme_ground=$1" \
        --set display/stream=false --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i \
        2>"$TEST_DIR/stderr" || driver=$?
    mock_stop_quiet
    if grep -a -q "invalid display.theme 'ember:dark'\|needs a libfypalette" \
            "$TEST_DIR/pty.out" "$TEST_DIR/stderr"; then
        skip "fyai is built without a libfypalette that sets a ground"
    fi
    if [ "$driver" -ne 0 ]; then
        tail -c 2000 "$TEST_DIR/pty.out" >&2
        fail "the $1 session did not show the answer"
    fi
    cp "$TEST_DIR/pty.out" "$CAPTURES/$1.out"
}

session theme
session terminal

"$PYTHON" - "$CAPTURES/theme.out" "$CAPTURES/terminal.out" <<'EOF' ||
import collections
import re
import sys

def raised(path):
    """The ground the card and the fenced block stand on: the background
    colour the answer uses most."""
    data = open(path, "rb").read()
    grounds = collections.Counter(
        tuple(int(v) for v in m.groups())
        for m in re.finditer(rb"48;2;(\d+);(\d+);(\d+)m", data))
    if not grounds:
        raise SystemExit(f"{path}: no palette background colour")
    return grounds.most_common(1)[0][0]

theme = raised(sys.argv[1])
terminal = raised(sys.argv[2])
# The raised ground of Ember is a warm neutral: red over green over blue.
if not theme[0] >= theme[1] >= theme[2]:
    raise SystemExit(f"the raised ground of the theme {theme} is not warm")
# Over rgb:1e1e/1e1e/2e2e it is a step above that background, and blue.
if not (terminal[2] > terminal[0] and max(terminal) < 90):
    raise SystemExit(f"the raised ground {terminal} did not follow the "
                     "background of the terminal")
EOF
    fail "the palette did not take the background of the terminal"

pass
