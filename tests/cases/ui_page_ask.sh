#!/bin/bash
# SPDX-License-Identifier: MIT
# Under display/renderer=page a question of ask_user takes the input area: its
# options are drawn above the prompt, the arrows move the selection, Enter
# accepts it, a number key or a click chooses, typed text answers freely and
# Escape answers nothing. A question of a sub-agent is drawn there too, with who
# asks.
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
        --set tools=true --set display/renderer=page \
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

# The tool result that the model was given.
answered()
{
    "$PYTHON" - "$TEST_DIR/requests.jsonl" "$1" <<'PY' ||
import json
import sys

want = sys.argv[2]
for line in open(sys.argv[1]):
    for m in json.loads(line)["body"]["messages"]:
        if m.get("role") == "tool" and m.get("tool_call_id") == "call_ask_1":
            if m.get("content") != want:
                raise SystemExit("answer %r, not %r" % (m.get("content"), want))
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
at = next(i for i, r in enumerate(rows) if "? Proceed with the mock plan?" in r)
want = ["  ? Proceed with the mock plan?", "  › 1. yes", "    2. no",
        "    or type an answer"]
if rows[at:at + 4] != want:
    raise SystemExit("question rows: %r" % rows[at:at + 4])
if not rows[at + 5].startswith("❯"):
    raise SystemExit("the prompt is not under the question: %r" % rows[at + 5])
PY
    fail "the page did not draw the question in the input area"
answered yes "Enter"
mock_stop 2

ask down "raw:1b5b42|wait-screen:› 2. no|raw:0d"
answered no "Down and Enter"
mock_stop 2

ask number "raw:32"
answered no "the key 2"
mock_stop 2

# Once text is typed the number keys type too.
ask typed "raw:6d|frame:2|raw:32|frame:2|raw:0d"
answered m2 "typed text"
mock_stop 2

ask escape "raw:1b"
answered "tool note: the user did not provide an answer" "Escape"
mock_stop 2

# A click on an option chooses it. The page stands under the transcript, so the
# row of the click on the screen is not the row of the option in the page; the
# tile controls grab the mouse.
CLICK=$(printf '\033[<0;7;8M\033[<0;7;8m' | od -An -tx1 | tr -d ' \n')
ask click "raw:$CLICK" --set display/work_controls=zoom
answered no "a click on the option"
mock_stop 2

# A sub-agent asks through its parent, whose page names it.
fyai_test_setup
mock_start agent_asks_user.json
driver=0
FYAI_PTY_COLS=100 FYAI_PTY_INPUT="ask a sub-agent to ask me" \
FYAI_PTY_NEEDLE="WHICH-COLOUR?" FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-screen:or type an answer|raw:32|wait-screen:Delegated and done." \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=20 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/renderer=page --set api=responses \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i || driver=$?
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the question of the sub-agent was not answered"
fi
grep -a -q "asked by [^ ]*asker" "$TEST_DIR/pty.out" ||
    fail "the page did not say which sub-agent asks"
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the answer never reached the sub-agent"
import json
import sys

reqs = [json.loads(l)["body"] for l in open(sys.argv[1])]
child = [r for r in reqs if "fyai sub-agent" in json.dumps(r)]
if len(child) < 2 or "green" not in json.dumps(child[-1]["input"]):
    raise SystemExit("the sub-agent was not given the answer")
PY
mock_stop 4

# Two sub-agents ask at once. The page shows one question and counts the other,
# which it shows when the first is answered.
fyai_test_setup
mock_start ui_page_ask_queue.json
driver=0
FYAI_PTY_COLS=100 FYAI_PTY_INPUT="ask two sub-agents" \
FYAI_PTY_NEEDLE="or type an answer" FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-screen:(1 more)|raw:31|wait-gone:(1 more)|wait-screen:or type an answer|raw:32|wait-screen:Both questions answered." \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=20 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set display/renderer=page \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
    driver=$?
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the two questions of the sub-agents were not answered"
fi
"$PYTHON" - "$TEST_DIR/pty.out" "$TEST_DIR/requests.jsonl" <<'PY' ||
import json
import sys

data = open(sys.argv[1], "rb").read()
first = {name: data.find(b"? %s-QUESTION?" % name.upper().encode())
         for name in ("alpha", "beta")}
if min(first.values()) < 0:
    raise SystemExit("a question was never drawn: %r" % first)
order = sorted(first, key=first.get)
answers = {}
for line in open(sys.argv[2]):
    for m in json.loads(line)["body"]["messages"]:
        call = str(m.get("tool_call_id", ""))
        if m.get("role") == "tool" and call.startswith("call_ask_"):
            answers[call[len("call_ask_"):]] = m.get("content")
want = {order[0]: order[0] + "-one", order[1]: order[1] + "-two"}
if answers != want:
    raise SystemExit("answers %r, not %r" % (answers, want))
PY
    fail "the queued questions were not answered in the order they were shown"
mock_stop 6

# A grandchild asks through its parent and the root, whose page names it.
fyai_test_setup
mock_start agent_recursive_question.json
driver=0
FYAI_PTY_COLS=100 FYAI_PTY_INPUT="delegate recursively" \
FYAI_PTY_NEEDLE="NESTED_COLOUR?" FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-screen:or type an answer|raw:32|wait-screen:Recursive delegation complete." \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=20 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set display/renderer=page --set api=responses \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i || driver=$?
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the question of the grandchild was not answered"
fi
grep -a -q "asked by [^ ]*agent:child/agent:grandchild" "$TEST_DIR/pty.out" ||
    fail "the page did not say which grandchild asks"
assert_request 3 '"green" in json.dumps(r["body"])'
mock_stop 6

pass
