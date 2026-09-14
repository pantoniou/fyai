#!/bin/bash
# SPDX-License-Identifier: MIT
# Under display/renderer=page the ladder of a tile selects the view of its
# page. A pane of five rows leaves a bang tile a screen too short for its
# head: it shows the screen alone. A pane of three rows leaves no screen worth
# reading: it shows the head that says whose call it is.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

# $1 is the pane height, $2 the steps that wait for the view to take effect.
run_at()
{
    rows=$1
    view=$2
    fyai_test_setup
    # The output is not a word of the command: the command stands in the
    # user card and in the head, and only the screen holds VIEWOUT.
    FYAI_TRACE="$TEST_DIR/trace.log" \
    FYAI_PTY_INPUT="!sh -c 'printf %s%s VIEW OUT; sleep 60'" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:Ctrl-]|$view|raw:1d|wait-gone:Ctrl-]|send:/status|wait-screen:Usage / total" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/renderer=page \
        --set "display/work_zoom_rows=$rows" -m mock-model -i || driver=$?
    cp "$TEST_DIR/pty.out" "$CAPTURES/$rows.out"
    cp "$TEST_DIR/trace.log" "$CAPTURES/$rows.trace" 2>/dev/null || :
}

# The screen view takes the head off the tile: its output stands without the
# name. The head view stands without the output.
# A build without page support says so and never takes the head off: look for
# that before the status of the driver, whose wait for the view then expires.
# A bang shell is not a turn, so the warning is not drained to the screen; the
# trace records it when it is raised.
driver=0
run_at 5 "wait-screen:VIEWOUT|wait-gone:bang-1"
if grep -a -q "needs a libfytimui" "$CAPTURES/5.out" "$CAPTURES/5.trace" \
        2>/dev/null; then
    skip "this build has no page support"
fi
[ "$driver" -eq 0 ] || fail "the five-row session did not run to its end"
run_at 3 "wait-screen:bang-1"
[ "$driver" -eq 0 ] || fail "the three-row session did not run to its end"

"$PYTHON" - "$CAPTURES/5.out" "$CAPTURES/3.out" "$TESTS_DIR" <<'PY' || \
    fail "the ladder did not select the view of the tile page"
import sys

sys.path.insert(0, sys.argv[3])
from tile_assert import frames

def live(path):
    # Frames in which the tile holds the keys: the way back is on the status
    # row, so the tile is live. The keys are taken back with Ctrl-] while the
    # program still runs, and the prompt then answers /status.
    return [f for f in frames(open(path, "rb").read(), 30, 100)
            if any("Ctrl-]" in row for row in f)]

def shows(frame, text):
    return any(text in row for row in frame)

short = live(sys.argv[1])
if not short:
    raise SystemExit("the five-row pane never held the keys")
if not any(shows(f, "VIEWOUT") and not shows(f, "bang-1") for f in short):
    raise SystemExit("a five-row pane did not show the screen alone")

tiny = live(sys.argv[2])
if not tiny:
    raise SystemExit("the three-row pane never held the keys")
if not any(shows(f, "bang-1") and not shows(f, "VIEWOUT") for f in tiny):
    raise SystemExit("a three-row pane did not show the head alone")
PY

pass
