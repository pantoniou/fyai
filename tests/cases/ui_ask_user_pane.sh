#!/bin/bash
# SPDX-License-Identifier: MIT
# The input pane changes shape while ask_user waits for an answer: the
# question is pinned above the prompt in place of the banner, its numbered
# options list as their own rows below it, and the marker switches from the
# default to "?". Both revert once the answer is in.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ask_user.json

FYAI_PTY_INPUT="ask me something" \
FYAI_PTY_NEEDLE="Proceed with the mock plan?" \
FYAI_PTY_AFTER="settle:0.4|send:yes|wait:User said yes, proceeding." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'EOF' || fail "the input pane did not change shape for the question"
import sys

# The band redraws in place with relative cursor motion, so raw byte
# adjacency cannot tell what row held what: replay the capture through the
# same screen model the PTY driver uses for the live input row, one chunk
# at a time, and look for the pending shape and then its later reversal -
# frame coalescing makes any single fixed byte offset unreliable.
sys.path.insert(0, sys.argv[2])
from screen import Screen

data = open(sys.argv[1], "rb").read()


def header_shown(rows):
    return any(r.strip() == "Proceed with the mock plan?" for r in rows)


def options_shown(rows):
    stripped = [r.strip() for r in rows]
    return "1  yes" in stripped and "2  no" in stripped


def banner_restored(rows):
    return "❯" in rows and any("mock-model" in r and "·" in r for r in rows)


screen = Screen(30, 100)
pending_ok = False
restored_ok = False
pos = 0
while pos < len(data):
    screen.feed(data[pos:pos + 1])
    pos += 1
    rows = [r for r in screen.display() if r.strip()]
    if not pending_ok:
        # The marker switched to "?", the question is pinned in the header
        # above the prompt, and its options list as their own rows below it
        # - distinct from the transcript's separate printout above all of
        # this.
        if "? yes" in rows and header_shown(rows) and options_shown(rows):
            pending_ok = True
    elif banner_restored(rows):
        restored_ok = True
        break

if not pending_ok:
    raise SystemExit("input pane never showed the '?' marker with the "
                      "question header and options list while the answer "
                      "was typed")
if not restored_ok:
    raise SystemExit("prompt marker/banner were never restored after the "
                      "answer")
EOF

mock_stop 2
pass
