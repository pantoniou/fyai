#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that focus-key handling returns the input-frame suffix.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# Send Ctrl-] and /status in one input frame.
FYAI_PTY_INPUT="!sh -c 'printf ZOOMED; sleep 5'" \
FYAI_PTY_NEEDLE="ZOOMED" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait:Ctrl-]|raw:1d2f7374617475730a|wait:Usage / total" \
FYAI_PTY_SNAPSHOT="$TEST_DIR/tail.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true -m mock-model -i

"$PYTHON" - "$TEST_DIR/tail.out" "$TESTS_DIR" <<'PYEOF' || \
    fail "the line typed with the focus key was lost"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

s = Screen(30, 100)
s.feed(open(sys.argv[1], "rb").read())
shown = "\n".join(s.lines())
if "Usage / total" not in shown:
    raise SystemExit("the prompt never ran the line typed after the key")
PYEOF

pass
