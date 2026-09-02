#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify parallel full-screen shells across successive resize barriers.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

CMD_A="!$PYTHON $TESTS_DIR/resize_tui.py A"
CMD_B="!$PYTHON $TESTS_DIR/resize_tui.py B"
AFTER="raw:1d|send:$CMD_B|wait:B SIZE|resize:24x80|drain:.2|resize:28x80|drain:.2|"
# Settle and request a complete repaint before reading the screen.
AFTER="${AFTER}resize:26x80|drain:.2|resize:32x80|drain:1|raw:0c|drain:1|raw:1d"
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="$CMD_A" FYAI_PTY_NEEDLE="A SIZE 24x98" \
FYAI_PTY_AFTER="$AFTER" FYAI_PTY_AFTER_PAUSE=.01 \
FYAI_PTY_AFTER_TIMEOUT=10 FYAI_PTY_SNAPSHOT="$TEST_DIR/multi.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true -m mock-model -i

"$PYTHON" - "$TEST_DIR/multi.out" "$TESTS_DIR" <<'PYEOF' || \
    fail "parallel bang shells retained different row grants after resize"
import re
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

data = open(sys.argv[1], "rb").read()
# Read the complete repaint following Ctrl-L.
clear = b"\x1b[0m\x1b[H\x1b[2J"
if clear in data:
    data = data[data.rfind(clear):]
s = Screen(32, 80)
s.feed(data)
sizes = {}
# Require both tile heads on one screen row.
for line in s.lines():
    for name, rows, cols in re.findall(r"([AB]) SIZE (\d+)x(\d+)", line):
        sizes[name] = (int(rows), int(cols))
if len(sizes) != 2:
    raise SystemExit("expected two visible terminal sizes: %r" % (sizes,))
if sizes["A"][0] != sizes["B"][0]:
    raise SystemExit("terminal row grants differ: %r" % (sizes,))
PYEOF

pass
