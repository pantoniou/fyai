#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify user ownership, sizing, and keyboard focus for a bang shell.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_bang_user_owned.json

ZOOM_AFTER="raw:1d|send:try to close my shell|"
ZOOM_AFTER="${ZOOM_AFTER}wait:User shell ownership preserved.|"
ZOOM_AFTER="${ZOOM_AFTER}send:/zoom bang-1|wait:focused|"
ZOOM_AFTER="${ZOOM_AFTER}send:echo SECOND-ZOOM|wait:SECOND-ZOOM|raw:1d"
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="!sh -c 'sleep .2; stty size; printf READY; sleep 10'" \
FYAI_PTY_NEEDLE="READY" \
FYAI_PTY_AFTER="$ZOOM_AFTER" \
FYAI_PTY_SNAPSHOT="$TEST_DIR/snapshot.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/work_max_rows=5 \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/snapshot.out" "$TESTS_DIR" <<'PYEOF' || \
    fail "bang shell ownership was not preserved"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

data = open(sys.argv[1], "rb").read()
s = Screen(30, 100)
s.feed(data)
lines = s.lines()
shown = "\n".join(lines)
# Inspect emitted data because Ctrl-] clears transient focus chrome.
if "focused · Ctrl-] returns to the prompt".encode() not in data:
    raise SystemExit("focused bang shell has no focus indication")
if "shell [bang-1]" not in shown:
    raise SystemExit("bang shell does not display its /zoom name")
# Confirm that the rejected close leaves the shell live.
if "READY" not in shown:
    raise SystemExit("user shell disappeared after model close request")
if "SECOND-ZOOM" not in shown:
    raise SystemExit("user shell stopped updating after its second zoom")
# Confirm that unzoom restores the configured pane cap.
import re
sizes = [(int(r), int(c)) for r, c in re.findall(rb"(\d+) (\d+)", data)]
if not sizes or max(r for r, _ in sizes) > 5:
    raise SystemExit("zoomed-out user shell escaped work_max_rows: %r" %
                     (sizes,))
PYEOF

assert_request 1 'any(m.get("role") == "tool" and "belongs to the user" in m.get("content", "") for m in r["body"]["messages"])'
mock_stop 2
pass
