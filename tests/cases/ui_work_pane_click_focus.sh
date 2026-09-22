#!/bin/bash
# SPDX-License-Identifier: MIT
# With mouse capture enabled, clicking a tile screen or header focuses that
# tile. Clicking outside all tiles focuses the prompt. Both renderers must
# handle the same click coordinates.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

# An SGR press and release at one-based column $1 and row $2, as hex.
click()
{
    printf '\033[<0;%d;%dM\033[<0;%d;%dm' "$1" "$2" "$1" "$2" |
        od -An -tx1 | tr -d ' \n'
}

hex()
{
    printf '%s' "$1" | od -An -tx1 | tr -d ' \n'
}

# Each tile prints a marker in its screen and then runs cat to expose received
# input. A click finds the marker. The second tile starts focused.
# The status row is outside every tile. The provider uses a closed loopback
# port so accidental prompt input cannot leave the machine.
run_with()
{
    renderer=$1
    fyai_test_setup
    FYAI_PTY_COLS=100 FYAI_PTY_ROWS=30 \
    FYAI_PTY_INPUT="!sh -c 'echo LEFT\"\"-SCREEN; exec cat'" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:Ctrl-]|raw:1d|wait-gone:Ctrl-]|"\
"send:!sh -c 'echo RIGHT\"\"-SCREEN; exec cat'|wait-screen:RIGHT-SCREEN|wait-screen:Ctrl-]|"\
"click:LEFT-SCREEN|send:LEFTKEYS|wait-screen:LEFTKEYS|"\
"click:RIGHT-SCREEN|send:RIGHTKEYS|wait-screen:RIGHTKEYS|"\
"click:[bang-1]|send:HEADKEYS|wait-screen:HEADKEYS|"\
"raw:$(click 10 30)|wait-gone:Ctrl-]|"\
"raw:$(hex PROMPTKEYS)|wait-screen:PROMPTKEYS|raw:15|wait-gone:PROMPTKEYS|"\
"send:/kill bang-1|wait-screen:stopping shell bang-1|"\
"send:/kill bang-2|wait-screen:stopping shell bang-2" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/work_controls=zoom \
        --set display/work_min_tile_cols=30 \
        --set "display/renderer=$renderer" \
        --set retry/max_attempts=1 \
        --set api=chat-completions \
        --set "api_url=http://127.0.0.1:9/v1/chat/completions" \
        -m mock-model -i ||
        fail "$renderer: a click did not change keyboard focus"
    cp "$TEST_DIR/pty.out" "$CAPTURES/$renderer.out"
}

run_with stack
run_with page

if grep -a -q "needs a libfytimui" "$CAPTURES/page.out"; then
    skip "this build has no page support"
fi

for renderer in stack page; do
    "$PYTHON" - "$CAPTURES/$renderer.out" "$renderer" <<'PYEOF' ||
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames

data = open(sys.argv[1], "rb").read()
shown = frames(data, 30, 100)


def column(needle):
    """The column the text was first shown at, beside the rule."""
    for disp in shown:
        for row in disp:
            at = row.find(needle)
            if at >= 0:
                return at
    raise SystemExit("%s: %s was never shown" % (sys.argv[2], needle))


# The left tile ends before the middle of the terminal, the right starts
# after it.
if column("LEFTKEYS") >= 50:
    raise SystemExit("clicking the left screen did not focus it")
if column("RIGHTKEYS") < 50:
    raise SystemExit("clicking the right screen did not focus it")
if column("HEADKEYS") >= 50:
    raise SystemExit("clicking the left header did not focus it")
if not any(row.lstrip().startswith("❯ PROMPTKEYS")
           for disp in shown for row in disp):
    raise SystemExit("clicking outside the tiles did not focus the prompt")
PYEOF
        fail "$renderer: keyboard focus moved to the wrong target"
done

pass
