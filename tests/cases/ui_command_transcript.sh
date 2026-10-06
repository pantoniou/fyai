#!/bin/bash
# SPDX-License-Identifier: MIT
# With display/command_output set to transcript, a slash command and its
# output are drawn and stored in the conversation. The next turn draws the
# fullscreen transcript again from storage, so both stay on the screen after
# it. With pane, the output goes to the pane and the transcript keeps nothing
# of the command.
set -eu
. "$(dirname "$0")/../harness.sh"

# run_steps STEPS [ARG...]: ask, run the steps, and end on the second answer.
run_steps() {
	local steps="$1"

	shift
	fyai_test_setup
	mock_start ui_card_separation.json
	FYAI_PTY_ROWS=60 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="first question" \
	FYAI_PTY_NEEDLE="banana" FYAI_PTY_AFTER="$steps" \
	"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	    "$FYAI_BIN" -k test-key --theme dark \
	    --set display/markdown=true --set display/stream=true \
	    --set display/screen=fullscreen \
	    --set api=chat-completions \
	    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i "$@"
	mock_stop 2
}

# final_rows: the rows of the last frame, without their trailing blanks.
final_rows() {
	"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY'
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

screen = Screen(60, 100)
screen.feed(open(sys.argv[1], "rb").read())
for row in screen.display():
    print(row.rstrip())
PY
}

# The output is drawn under the card, with no heading, and both stay after
# the next turn.
run_steps \
	"send:/status|wait-screen:Auth / provider|send:yellow?|wait-screen:second" \
	--set display/command_output=transcript
final_rows > "$TEST_DIR/transcript.rows"
"$PYTHON" - "$TEST_DIR/transcript.rows" <<'PY' ||
import sys

rows = open(sys.argv[1]).read().split("\n")


def index(text):
    for i, row in enumerate(rows):
        if text in row:
            return i
    raise SystemExit("%r is not on the screen" % text)


card = index("│ /status")
output = index("Auth / provider")
after = index("│ yellow?")
if not card < output < after:
    raise SystemExit("rows out of order: card %d, output %d, next %d" %
                     (card, output, after))
# The card names the command: no heading stands over its output.
for row in rows[card + 1:output]:
    if row.strip().endswith(" status"):
        raise SystemExit("a heading stands over the output: %r" % row)
PY
	fail "the command is not in the transcript"

# pane, the default: the output is in the popup, and the transcript keeps
# nothing of the command, in no frame.
run_steps \
	"send:/status|wait-screen:Auth / provider|raw:1b|wait-gone:Auth / provider|send:yellow?|wait-screen:second"
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

data = open(sys.argv[1], "rb").read()
screen = Screen(60, 100)
end = b"\x1b[?2026l"
pos = 0
while True:
    at = data.find(end, pos)
    if at < 0:
        break
    screen.feed(data[pos:at + len(end)])
    pos = at + len(end)
    for row in screen.display():
        if "│ /status" in row:
            raise SystemExit("the card of the command was drawn: %r" % row)
rows = screen.display()
if not any("│ yellow?" in row for row in rows):
    raise SystemExit("the next question is not on the screen")
if any("Auth / provider" in row for row in rows):
    raise SystemExit("pane output was drawn into the transcript")
PY
	fail "a command with pane output reached the transcript"

pass
