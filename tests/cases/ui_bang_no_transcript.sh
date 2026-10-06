#!/bin/bash
# SPDX-License-Identifier: MIT
# A bang command is not a part of the conversation. Its line is not drawn as
# a card, and when its program ends the tile keeps the output, its buttons and
# the keys, and the status says that Escape closes it: nothing of it reaches
# the transcript or the arena.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
driver=0
FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="!printf BANG%sOUT X" \
FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait-screen:BANGXOUT|wait-screen:Esc closes|wait-screen:×|raw:1b|wait-gone:BANGXOUT" \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true \
    --set display/work_controls=full -m mock-model -i || driver=$?
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the bang tile did not keep its output and go when dismissed"
fi

"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

screen = Screen(30, 100)
screen.feed(open(sys.argv[1], "rb").read())
kept = [r for r in screen.lines() + screen.erased
        if "BANGXOUT" in r or "!printf" in r]
if kept:
    raise SystemExit("the bang reached the transcript: %r" % kept)
PY
    fail "a bang command left rows in the transcript"
if "$FYAI_BIN" dump state 2>&1 | grep -q 'BANG'; then
    fail "a bang command was stored"
fi
pass
