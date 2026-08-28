#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify zoomed terminal input, pane ownership, and result capture.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start session_zoom.json

FYAI_PTY_INPUT="open a shell" FYAI_PTY_NEEDLE="the shell is open." \
FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER='send:/zoom box|wait:focused|send:echo FROM-THE-USER|raw:1d|send:read it back|wait:read the session.' \
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
if "focused · Ctrl-] returns to the prompt".encode() not in plain:
    raise SystemExit("the focused session had no focus indication")

# Require user input on the program screen, not the prompt.
shown = frames(data, 30, 100)
typed = [i for i, disp in enumerate(shown)
         if any("FROM-THE-USER" in row for row in disp)]
if not typed:
    raise SystemExit("what the user typed never reached the session screen")

# Require the prompt to disappear while the tile owns input.
def has_prompt(disp):
    return any(row.strip().startswith("\u276f") for row in disp)

if not has_prompt(shown[0]):
    raise SystemExit("no prompt before the zoom")
if has_prompt(shown[typed[0]]):
    raise SystemExit("the prompt was still drawn while the tile had the keys")
if not has_prompt(shown[-1]):
    raise SystemExit("the prompt did not come back after Ctrl-]")

# Pane geometry is covered by the libfytimui work-pane zoom test.
PYEOF

# Require user-entered terminal input in the model result.
assert_request 3 \
	'any("FROM-THE-USER" in m.get("content", "") for m in r["body"]["messages"] '\
'if m.get("role") == "tool")'

mock_stop 4
pass
