#!/bin/bash
# SPDX-License-Identifier: MIT
# A question of ask_user takes the input area: its
# options are drawn above the prompt, the arrows move the selection, Enter
# accepts it, a number key or a click chooses, typed text answers freely and
# Escape answers nothing.
set -eu
. "$(dirname "$0")/../harness.sh"

# Ask under the page and drive the answer with the after script $2; further
# arguments go to fyai.
ask()
{
    name=$1
    fyai_test_setup
    mock_start ask_user.json
    driver=0
    FYAI_TRACE="$TEST_DIR/trace.log" \
    FYAI_PTY_COLS=100 FYAI_PTY_INPUT="ask me something" \
    FYAI_PTY_NEEDLE="Proceed with the mock plan?" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:or type an answer|$2|wait-screen:User said yes" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/stream=false \
        --set tools=true \
        --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model \
        "${@:3}" -i ||
        driver=$?
    if grep -a -q "needs a libfytimui" "$TEST_DIR/pty.out" "$TEST_DIR/trace.log"; then
        skip "this build has no page support"
    fi
    if [ "$driver" -ne 0 ]; then
        tail -c 2000 "$TEST_DIR/pty.out" >&2
        fail "the question was not answered by $name"
    fi
}

# The tool result that the model was given: "sel:LABEL", "other:TEXT" or
# "declined".
answered()
{
    "$PYTHON" - "$TEST_DIR/requests.jsonl" "$1" <<'PY' ||
import json
import sys

want = sys.argv[2]
for line in open(sys.argv[1]):
    for m in json.loads(line)["body"]["messages"]:
        if m.get("role") == "tool" and m.get("tool_call_id") == "call_ask_1":
            res = json.loads(m["content"])
            if want == "declined":
                got = res["status"]
            else:
                a = res["answers"][0]
                got = ("sel:" + ",".join(a["selected"]) if a["selected"]
                       else "other:" + a.get("other", ""))
            if got != want:
                raise SystemExit("answer %r, not %r" % (got, want))
            raise SystemExit(0)
raise SystemExit("the model was not given an answer")
PY
        fail "$2 did not give the answer '$1'"
}

ask enter "raw:0d"
# The page drew the question and its options in the input area.
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
data = open(sys.argv[1], "rb").read()
screen = Screen(30, 100)
pos = 0
rows = None
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    if any("or type an answer" in r for r in screen.display()):
        rows = [r.rstrip() for r in screen.display()]
        break
if rows is None:
    raise SystemExit("the question was never drawn")
at = next(i for i, r in enumerate(rows) if "Proceed with the mock plan?" in r)
want = ["  Plan  Proceed with the mock plan?", "  ▌ 1. yes",
        "  ▌    The yes choice", "    2. no", "       The no choice",
        "    or type an answer"]
if rows[at:at + 6] != want:
    raise SystemExit("question rows: %r" % rows[at:at + 6])
if "❯" not in rows[at + 7]:
    raise SystemExit("the prompt is not under the question: %r" % rows[at + 7])
PY
    fail "the page did not draw the question in the input area"
answered sel:yes "Enter"
mock_stop 2
# The transcript shows the answer as the user would read it, not the JSON the
# model was given.
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

screen = Screen(30, 100)
screen.feed(open(sys.argv[1], "rb").read())
rows = [r.rstrip() for r in screen.display()]
text = "\n".join(rows)
if '"status"' in text or '"answers"' in text:
    raise SystemExit("the answer is shown as JSON:\n" + text)
at = [i for i, r in enumerate(rows) if "Plan Proceed with the mock plan?" in r]
if not at or "\u2192 yes" not in rows[at[0] + 1]:
    raise SystemExit("the answer is not shown under its question:\n" + text)
PY
    fail "the transcript did not show the answer for the user"

ask down "raw:1b5b42|wait-screen:▌ 2. no|raw:0d"
answered sel:no "Down and Enter"
mock_stop 2

ask number "raw:32"
answered sel:no "the key 2"
mock_stop 2

# Once text is typed the number keys type too.
ask typed "raw:6d|frame:2|raw:32|frame:2|raw:0d"
answered other:m2 "typed text"
mock_stop 2

ask escape "raw:1b"
answered declined "Escape"
mock_stop 2

# A click on an option chooses it; the tile controls grab the mouse.
ask click "click:2. no" --set display/work_controls=zoom
answered sel:no "a click on the option"
mock_stop 2

pass
