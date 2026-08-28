#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify named and fixed zoom-row policies.
set -eu
. "$(dirname "$0")/../harness.sh"

run_size()
{
    policy=$1
    expected=$2
    fyai_test_setup
    FYAI_PTY_ROWS=32 FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="!sh -c 'sleep 1; stty size; sleep 8'" \
    FYAI_PTY_NEEDLE="bang-1" \
    FYAI_PTY_AFTER="send:/zoom bang-1|wait:focused|wait: 98|drain:.5|raw:0c|drain:1|raw:1d" \
    FYAI_PTY_SNAPSHOT="$TEST_DIR/zoom.out" \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
        --set "display/work_zoom_rows=$policy" -m mock-model -i
    "$PYTHON" - "$TEST_DIR/zoom.out" "$expected" "$TESTS_DIR" <<'PYEOF'
import re
import sys

sys.path.insert(0, sys.argv[3])
from screen import Screen

# Read the granted row count from the program screen.
data = open(sys.argv[1], "rb").read()
clear = b"\x1b[0m\x1b[H\x1b[2J"
if clear in data:
    data = data[data.rfind(clear):]
s = Screen(32, 100)
s.feed(data)
sizes = [m for line in s.lines() for m in re.findall(r"\b(\d+) (\d+)\b", line)]
if not sizes:
    raise SystemExit("the zoomed shell did not report its size")
rows, _ = map(int, sizes[-1])
if rows != int(sys.argv[2]):
    raise SystemExit("zoomed shell has %d rows; expected %s" %
                     (rows, sys.argv[2]))
PYEOF
}

# The cap includes surrounding UI and shell chrome.
run_size quarter 5
run_size 12 9
pass
