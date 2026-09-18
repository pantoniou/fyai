#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify user ownership, sizing, and keyboard focus for a bang shell.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_bang_user_owned.json

# The shell writes READY in two parts, so only its output holds the word and
# not the command in the head, then reports its size and a count every 0.2
# seconds. A report made before the tile was granted its rows names the whole
# terminal, so the case waits for the granted size. The status hint says that
# a tile holds the keys: the notice of /zoom names Ctrl-] too.
ZOOM_AFTER="wait-screen:READY|wait-screen:SIZE 3 98|wait-screen:Ctrl-] returns to the prompt|raw:1d|wait-gone:Ctrl-] returns to the prompt|"
ZOOM_AFTER="${ZOOM_AFTER}send:try to close my shell|"
ZOOM_AFTER="${ZOOM_AFTER}wait-screen:User shell ownership preserved.|"
ZOOM_AFTER="${ZOOM_AFTER}send:/zoom bang-1|wait-screen:Ctrl-] returns to the prompt|"
ZOOM_AFTER="${ZOOM_AFTER}send:echo SECOND-ZOOM|wait-screen:SECOND-ZOOM|"
ZOOM_AFTER="${ZOOM_AFTER}raw:1d|wait-gone:Ctrl-] returns to the prompt"
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="!sh -c 'printf \"%s%s\\n\" REA DY; n=0; while :; do n=\$((n+1)); echo \"SIZE \$(stty size) #\$n\"; sleep 0.2; done'" \
FYAI_PTY_NEEDLE="bang-1" \
FYAI_PTY_AFTER="$ZOOM_AFTER" \
FYAI_PTY_SNAPSHOT="$TEST_DIR/snapshot.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/work_max_rows=6 \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/snapshot.out" "$TESTS_DIR" "$TEST_DIR/pty.out" <<'PYEOF' || \
    fail "bang shell ownership was not preserved"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

data = open(sys.argv[1], "rb").read()
s = Screen(30, 100)
s.feed(data)
lines = s.lines()
shown = "\n".join(lines)
# The way back is on the status row, which the tile does not grow a line for.
# Inspect emitted data because Ctrl-] clears it again.
if "Ctrl-] returns to the prompt".encode() not in data:
    raise SystemExit("focused bang shell has no focus indication")
if "shell [bang-1]" not in shown:
    raise SystemExit("bang shell does not display its /zoom name")
# Confirm that the shell was sized under the configured pane cap. The newest
# report of a frame is the size the shell has; from its granted size until the
# zoom, which gives it the pane, it stays under the cap.
import re
END = b"\x1b[?2026l"
SIZE = re.compile(r"SIZE (\d+) (\d+) #(\d+)")
capture = open(sys.argv[3], "rb").read()
frames = Screen(30, 100)
pos = 0
sizes = []
granted = zoomed = False
ready = echoed = False
while True:
    i = capture.find(END, pos)
    if i < 0:
        break
    frames.feed(capture[pos:i + len(END)])
    pos = i + len(END)
    reports = [(int(m.group(3)), int(m.group(1)), int(m.group(2)))
               for line in frames.display() for m in SIZE.finditer(line)]
    zoomed = zoomed or any("/zoom bang-1" in line for line in frames.display())
    if reports and not zoomed:
        newest = max(reports)[1:]
        granted = granted or newest == (3, 98)
        if granted:
            sizes.append(newest)
    ready = ready or any("READY" in line for line in frames.display())
    echoed = echoed or any("SECOND-ZOOM" in line for line in frames.display())
# The shell drew its output, and after the rejected close it still echoed what
# the second zoom typed into it.
if not ready:
    raise SystemExit("user shell never drew its output")
if not echoed:
    raise SystemExit("user shell stopped updating after its second zoom")
if not sizes or max(r for r, _ in sizes) > 6:
    raise SystemExit("zoomed-out user shell escaped work_max_rows: %r" %
                     (sizes,))
PYEOF

assert_request 1 'any(m.get("role") == "tool" and "belongs to the user" in m.get("content", "") for m in r["body"]["messages"])'
mock_stop 2
pass
