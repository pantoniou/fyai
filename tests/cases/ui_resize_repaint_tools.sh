#!/bin/bash
# SPDX-License-Identifier: MIT
# A settled width change repaints the newest exchanges from what is stored.
# What a tool printed is drawn bounded by its preview limit, so the source it
# has is not the height it takes: an exchange measured by its source spends
# the whole screen on one turn and the rest of the transcript is not painted.
# The card for what the user said belongs to that repaint with the answer
# under it, and not to the screen before it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_resize_repaint_tools.json

FYAI_PTY_INPUT="turn 1 please" \
FYAI_PTY_ROWS=40 FYAI_PTY_COLS=100 \
FYAI_PTY_NEEDLE="Turn 1 answered." \
FYAI_PTY_AFTER="send:turn 2 please|wait:Turn 2 answered.|send:turn 3 please|wait:Turn 3 answered.|resize:72|wait:Turn 3 answered." \
FYAI_PTY_AFTER_TIMEOUT=20 \
FYAI_PTY_AFTER_PAUSE="1.5" \
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
# The repaint clears the screen and paints from the stored transcript. What
# follows that clear is the repaint, and the reading is taken there.
at = data.rfind(b"\x1b[2J")
if at < 0:
    raise SystemExit("the width change was never repainted")
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data[at:])

# A forty-row screen holds more than the newest exchange: what a tool printed
# is six rows of it and not sixty.
for needle in (b"turn 2 please", b"T2-LINE-", b"Turn 2 answered."):
    if needle not in plain:
        raise SystemExit("the repaint painted one exchange only: %r" % needle)

# Each exchange keeps its own order: what was asked, then what ran, then the
# answer. A card drawn straight to the screen while the rest is still in the
# sink puts every card above every answer.
order = [b"turn 2 please", b"T2-LINE-1", b"Turn 2 answered.",
         b"turn 3 please", b"T3-LINE-1", b"Turn 3 answered."]
at = 0
for needle in order:
    found = plain.find(needle, at)
    if found < 0:
        raise SystemExit("the repaint reordered the transcript: %r" % needle)
    at = found + len(needle)

# The bounded tool body is replayed as the preview it was, not in full.
if b"T2-LINE-30" in plain:
    raise SystemExit("the replayed tool body was not bounded")
EOF

pass
