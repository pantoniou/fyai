#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify stored tool-exchange repaint with preview limits and ordering.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_resize_repaint_tools.json

FYAI_PTY_INPUT="turn 1 please" \
FYAI_PTY_ROWS=40 FYAI_PTY_COLS=100 \
FYAI_PTY_NEEDLE="Turn 1 answered." \
FYAI_PTY_AFTER="send:turn 2 please|wait:Turn 2 answered.|send:turn 3 please|wait:Turn 3 answered.|resize:72|wait-frame:Turn 3 answered." \
FYAI_PTY_AFTER_TIMEOUT=20 \
FYAI_PTY_AFTER_PAUSE=".5" \
FYAI_PTY_TIMEOUT=60 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=true \
    --set tools=true --set approval=never --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || fail "the repaint dropped the tool exchanges"
import re
import sys

data = open(sys.argv[1], "rb").read()
# Inspect output after the repaint clears the screen.
at = data.rfind(b"\x1b[2J")
if at < 0:
    raise SystemExit("the width change was never repainted")
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data[at:])

# Require the second exchange within the preview-limited screen budget.
for needle in (b"turn 2 please", b"T2-LINE-", b"Turn 2 answered."):
    if needle not in plain:
        raise SystemExit("the repaint painted one exchange only: %r" % needle)

# Require question, tool output, and answer order for each exchange.
order = [b"turn 2 please", b"T2-LINE-1", b"Turn 2 answered.",
         b"turn 3 please", b"T3-LINE-1", b"Turn 3 answered."]
at = 0
for needle in order:
    found = plain.find(needle, at)
    if found < 0:
        raise SystemExit("the repaint reordered the transcript: %r" % needle)
    at = found + len(needle)

# Reject output beyond the configured tool preview.
if b"T2-LINE-30" in plain:
    raise SystemExit("the replayed tool body was not bounded")
EOF

pass
