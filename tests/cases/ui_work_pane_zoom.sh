#!/bin/bash
# SPDX-License-Identifier: MIT
# A zoomed tile is a real terminal.
#
# /zoom gives one live screen the whole work pane and the keys, so what the
# user types goes to the program rather than to the prompt. The program echoes
# it, so it reaches the emulator and the line log with it: what the user does
# in a zoomed session is part of the tool result the model is given. Ctrl-\ is
# the way back, because Escape and ^C belong to the program while it holds the
# keys.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start session_zoom.json

FYAI_PTY_INPUT="open a shell" FYAI_PTY_NEEDLE="the shell is open." \
FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER='send:/zoom box|wait:Ctrl-]|send:echo FROM-THE-USER|raw:1d|wait:grid|send:read it back|wait:read the session.' \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set shell/input_poll_ms=0 \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'PYEOF' || fail "the zoomed tile is wrong"
import os
import re
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)

if b"typing into box" not in plain:
    raise SystemExit("/zoom did not give the keys to the session")
if b"back to its grid" not in plain:
    raise SystemExit("Ctrl-] did not take the keys back")

# What the user typed went to the PROGRAM: it is on the session's screen,
# which is the tile, and not into the prompt the user typed away from.
shown = frames(data, 30, 100)
typed = [i for i, disp in enumerate(shown)
         if any("FROM-THE-USER" in row for row in disp)]
if not typed:
    raise SystemExit("what the user typed never reached the session screen")

# While the tile held the keys the prompt stood aside: a prompt that cannot
# be typed into is a row taken from the program. It is there before the zoom,
# gone while the program has the keys, and back once Ctrl-] returns them.
def has_prompt(disp):
    return any(row.strip().startswith("\u276f") for row in disp)

if not has_prompt(shown[0]):
    raise SystemExit("no prompt before the zoom")
if has_prompt(shown[typed[0]]):
    raise SystemExit("the prompt was still drawn while the tile had the keys")
if not has_prompt(shown[-1]):
    raise SystemExit("the prompt did not come back after Ctrl-]")

# That a zoomed tile takes the whole pane is asserted on terminal cells in
# libfytimui's own suite (fytim.workpane.vt.a_zoomed_tile_owns_the_rows),
# where a second tile exists to be displaced. This case has one program, so
# there is nothing here for the zoom to take the room from.
PYEOF

# The model is told what the user did: the session reading it was given
# carries the command the user typed into the program.
assert_request 3 \
	'any("FROM-THE-USER" in m.get("content", "") for m in r["body"]["messages"] '\
'if m.get("role") == "tool")'

mock_stop 4
pass
