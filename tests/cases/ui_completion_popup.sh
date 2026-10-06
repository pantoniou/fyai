#!/bin/bash
# SPDX-License-Identifier: MIT
# Tab on a slash command with several completions opens a popup above the
# prompt: each row is a command and its title. Down selects the next, and
# Enter puts it into the line without submitting it; a click on a row takes
# it. The popup is a layer:
# the prompt and the status stay in their rows.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json
driver=0
FYAI_PTY_ROWS=24 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="first question" \
FYAI_PTY_NEEDLE="Hello" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait-screen:Hello from the mock provider.|raw:2f627261|raw:09|wait-screen:open the branch browser|wait-screen:list, create, and manage branches|raw:1b5b42|raw:0d|wait-gone:open the branch browser|wait-screen:/branches|raw:7f7f7f7f7f7f7f7f7f|wait-gone:/branches|raw:2f627261|raw:09|wait-screen:open the branch browser|click:open the branch browser|wait-gone:open the branch browser|wait-screen:/branches|raw:7f7f7f7f7f7f7f7f7f|wait-gone:/branches" \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/screen=fullscreen \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
    driver=$?
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the completion popup did not go as the case says"
fi
mock_stop 1
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
data = open(sys.argv[1], "rb").read()
screen = Screen(24, 100)
pos = 0
before = None
popup = None
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    rows = screen.display()
    prompt = next((y for y, r in enumerate(rows) if "/bra" in r and
                   "open the branch" not in r), None)
    if prompt is None:
        continue
    pop = next((y for y, r in enumerate(rows) if "open the branch" in r), None)
    if pop is None and before is None:
        before = (prompt, rows[prompt + 1:])
    if pop is not None and popup is None:
        popup = (prompt, pop, rows[prompt + 1:])
if before is None or popup is None:
    sys.exit("no frame of the line with and without the popup")
if popup[1] >= popup[0]:
    sys.exit("the popup is not above the prompt")
# The completion slot under the prompt shows the keys of the popup; the
# status under it keeps its row.
if popup[0] != before[0] or popup[2][-1] != before[1][-1]:
    sys.exit("the popup moved the prompt or the status")
if not any("Enter take" in r for r in popup[2]):
    sys.exit("the completion slot does not name the keys of the popup")
PY
    fail "the popup is not a layer over the page"
pass
