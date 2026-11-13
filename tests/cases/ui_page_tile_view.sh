#!/bin/bash
# SPDX-License-Identifier: MIT
# The ladder of a tile selects the view of its
# page. A pane of five rows leaves a bang tile a screen too short for its
# head: it shows the screen alone. A pane of three rows leaves no screen worth
# reading: it shows the head that says whose call it is.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
fyai_test_on_exit 'rm -rf "$CAPTURES"'

# run_at ROWS < STEPS: the steps wait for the view to take effect.
run_at()
{
    local steps

    rows=$1
    steps="$(cat)"
    fyai_test_setup
    # The output is not a word of the command: the command stands in the
    # user card and in the head, and only the screen holds VIEWOUT.
    {
        cat <<'PTY'
cols: 100
timeout: 20
wait_timeout: 10
steps:
  - type: "!sh -c 'printf %s%s VIEW OUT; sleep 60'"
  - wait: bang-1
    timeout: session
  - wait-screen: "Ctrl-]"
PTY
        printf '%s\n' "$steps"
        cat <<'PTY'
  - write: "\x1d"
  - wait-gone: "Ctrl-]"
  - type: "/status"
  - wait-screen: Usage / total
PTY
    } | FYAI_TRACE="$TEST_DIR/trace.log" pty_run "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
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
run_at 5 <<'PTY'
  - wait-screen: VIEWOUT
  - wait-gone: bang-1
PTY
if grep -a -q "needs a libfytimui" "$CAPTURES/5.out" "$CAPTURES/5.trace" \
        2>/dev/null; then
    skip "this build has no page support"
fi
[ "$driver" -eq 0 ] || fail "the five-row session did not run to its end"
run_at 3 <<'PTY'
  - wait-screen: bang-1
PTY
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
