#!/bin/bash
# SPDX-License-Identifier: MIT
# With mouse capture enabled, clicking a tile screen or header focuses that
# tile. Clicking outside all tiles focuses the prompt. The page draws its
# tiles, so a click resolves through the slot of the page.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
fyai_test_on_exit 'rm -rf "$CAPTURES"'

# Each tile prints a marker in its screen and then runs cat to expose received
# input. A click finds the marker. The second tile starts focused.
# The status row is outside every tile. The provider uses a closed loopback
# port so accidental prompt input cannot leave the machine.
run_with()
{
    renderer=$1
    fyai_test_setup
    pty_run "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/work_controls=zoom \
        --set display/work_min_tile_cols=30 \
        --set retry/max_attempts=1 \
        --set api=chat-completions \
        --set "api_url=http://127.0.0.1:9/v1/chat/completions" \
        -m mock-model -i <<'PTY' ||
rows: 30
cols: 100
timeout: 20
wait_timeout: 10
steps:
  - type: "!sh -c 'echo LEFT\"\"-SCREEN; exec cat'"
  - wait: bang-1
    timeout: session
  - wait-screen: "Ctrl-]"
  - write: "\x1d"
  - wait-gone: "Ctrl-]"
  - type: "!sh -c 'echo RIGHT\"\"-SCREEN; exec cat'"
  - wait-screen: RIGHT-SCREEN
  - wait-screen: "Ctrl-]"
  - click: LEFT-SCREEN
  - type: LEFTKEYS
  - wait-screen: LEFTKEYS
  - click: RIGHT-SCREEN
  - type: RIGHTKEYS
  - wait-screen: RIGHTKEYS
  - click: "[bang-1]"
  - type: HEADKEYS
  - wait-screen: HEADKEYS
  - write: "\e[<0;10;30M\e[<0;10;30m"
  - wait-gone: "Ctrl-]"
  - write: "PROMPTKEYS"
  - wait-screen: PROMPTKEYS
  - write: "\x15"
  - wait-gone: PROMPTKEYS
  - type: "/kill bang-1"
  - wait-screen: stopping shell bang-1
  - type: "/kill bang-2"
  - wait-screen: stopping shell bang-2
PTY
        fail "$renderer: a click did not change keyboard focus"
    cp "$TEST_DIR/pty.out" "$CAPTURES/$renderer.out"
}

run_with page

if grep -a -q "needs a libfytimui" "$CAPTURES/page.out"; then
    skip "this build has no page support"
fi

"$PYTHON" - "$CAPTURES/page.out" <<'PYEOF' ||
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
    raise SystemExit("%s was never shown" % needle)


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
    fail "keyboard focus moved to the wrong target"

pass
