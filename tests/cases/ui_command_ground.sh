#!/bin/bash
# SPDX-License-Identifier: MIT
# The output of a slash command in the transcript stands on a ground of its
# own: display/command_bg theme mixes a little of the theme focus colour into
# the background of the terminal, so the ground is close to that background
# and not the same. none draws no ground.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

session()
{
    label=$1
    shift
    fyai_test_setup
    mock_start ui_card_separation.json
    driver=0
    # The ground is compared as RGB, which a 256-colour terminal cannot draw.
    COLORTERM=truecolor FYAI_PTY_BACKGROUND=rgb:1e1e/1e1e/2e2e \
    FYAI_PTY_ROWS=40 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="first question" \
    FYAI_PTY_NEEDLE="banana" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="send:/status|wait-screen:Auth / provider|snapshot" \
    FYAI_PTY_SNAPSHOT="$TEST_DIR/status.out" \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --color on --set display/markdown=true \
        --set display/theme=ember:dark --set display/theme_ground=terminal \
        --set display/screen=fullscreen \
        --set display/command_output=transcript "$@" \
        --set display/stream=true --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i \
        2>"$TEST_DIR/stderr" || driver=$?
    mock_stop_quiet
    if grep -a -q "invalid display.theme 'ember:\|needs a libfypalette\|needs a libfytimui" \
            "$TEST_DIR/pty.out" "$TEST_DIR/stderr"; then
        skip "this build has no fullscreen palette support"
    fi
    [ "$driver" -eq 0 ] || fail "the $label session did not show /status"
    cp "$TEST_DIR/status.out" "$CAPTURES/$label.out"
}

session theme
session none --set display/command_bg=none

"$PYTHON" - "$CAPTURES" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen


def ground(label):
    s = Screen(40, 100)
    s.feed(open("%s/%s.out" % (sys.argv[1], label), "rb").read())
    return s.ground_at("Auth / provider")


g = ground("theme")
if not g or len(g) != 3:
    raise SystemExit("theme: the output has no RGB ground: %r" % (g,))
terminal = (0x1e, 0x1e, 0x2e)
if g == terminal:
    raise SystemExit("theme: the ground is the background of the terminal")
if max(abs(a - b) for a, b in zip(g, terminal)) > 32:
    raise SystemExit("theme: the ground %r is far from the background %r" %
                     (g, terminal))
g = ground("none")
if g is not None:
    raise SystemExit("none: the output has a ground: %r" % (g,))
PY
    fail "the ground of the command output is wrong"

pass
