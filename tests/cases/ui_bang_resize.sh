#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that resizing a live bang session resizes its child PTY.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# Keep waiting after SIGWINCH and read the reported width from the screen.
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="!sh -c 'trap \"stty size | sed s/.*[[:space:]]/COLS:/\" WINCH; printf READY; sleep 30 & p=\$!; until wait \$p; do :; done'" \
FYAI_PTY_NEEDLE="READY" \
FYAI_PTY_AFTER="raw:14|wait:Ctrl-]|resize:72|drain:.3|raw:1d|drain:.3|raw:0c|drain:.5" \
FYAI_PTY_AFTER_TIMEOUT=20 FYAI_PTY_SNAPSHOT="$TEST_DIR/resize.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true -m mock-model -i

"$PYTHON" - "$TEST_DIR/resize.out" "$TESTS_DIR" <<'PYEOF' || \
    fail "resized bang terminal did not receive its inner work-pane size"
import re
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

data = open(sys.argv[1], "rb").read()
# Read the complete repaint following Ctrl-L.
clear = b"\x1b[0m\x1b[H\x1b[2J"
if clear in data:
    data = data[data.rfind(clear):]
s = Screen(30, 72)
s.feed(data)
cols = [int(c) for line in s.lines() for c in re.findall(r"COLS:(\d+)", line)]
if 70 not in cols:
    raise SystemExit("SIGWINCH size did not include 70 inner columns: %r" %
                     (cols,))
PYEOF

pass
