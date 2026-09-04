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
    # Report the size in a loop, not once after a fixed sleep: the driver
    # ends the session with /exit once the zoomed grid can be read, which
    # kills the loop. A sleep only sets how long the leg takes, and a
    # fixed second report races the policy change on a slow runner.
    FYAI_PTY_ROWS=32 FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="!sh -c 'while :; do stty size; sleep 0.2; done'" \
    FYAI_PTY_NEEDLE="bang-1" \
    FYAI_PTY_AFTER="wait:Ctrl-]|raw:1d|send:/zoom bang-1|wait:Ctrl-]|"\
"wait: 98|drain:.5|raw:0c|drain:1|raw:1d" \
    FYAI_PTY_SNAPSHOT="$TEST_DIR/zoom-$policy.out" \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty-$policy.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
        --set "display/work_zoom_rows=$policy" -m mock-model -i
    "$PYTHON" - "$TEST_DIR/zoom-$policy.out" "$expected" "$TESTS_DIR" <<'PYEOF'
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

run_focused_size()
{
    policy=$1
    expected=$2
    fyai_test_setup
    FYAI_PTY_ROWS=32 FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="!sh -c 'while :; do stty size; sleep 0.2; done'" \
    FYAI_PTY_NEEDLE="bang-1" \
    FYAI_PTY_AFTER="wait:Ctrl-]|wait: 98|drain:.5|raw:1d" \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/focused.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
        --set "display/work_zoom_rows=$policy" -m mock-model -i
    "$PYTHON" - "$TEST_DIR/focused.out" "$expected" <<'PYEOF'
import re
import sys

sizes = re.findall(rb"\b(\d+) (\d+)\b", open(sys.argv[1], "rb").read())
if not sizes:
    raise SystemExit("the focused shell did not report its size")
rows, _ = map(int, sizes[-1])
if rows != int(sys.argv[2]):
    raise SystemExit("focused shell has %d rows; expected %s" %
                     (rows, sys.argv[2]))
PYEOF
}

run_key_cycle()
{
    fyai_test_setup
    FYAI_PTY_ROWS=32 FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="!sh -c 'while :; do stty size; sleep 0.2; done'" \
    FYAI_PTY_NEEDLE="bang-1" \
    FYAI_PTY_AFTER="wait:14 98|raw:1b5b3131363b3675|"\
"wait:work pane height: quarter|wait:6 98|raw:1d" \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/cycle.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
        --set display/work_zoom_rows=half -m mock-model -i
}

# The cap includes surrounding UI and shell chrome. The prompt block stands
# whether the tile holds the keys or not, so these do not change with focus.
run_focused_size half 14
run_key_cycle
run_size half 14
run_size quarter 6
run_size 12 10
pass
