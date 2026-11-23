#!/bin/bash
# SPDX-License-Identifier: MIT
# display/screen=fullscreen puts the page on the alternate screen. The
# transcript is a view of the page: it shows the exchanges as they are made,
# PageUp takes it back to the first of them and PageDown to the end, and the
# terminal gets its own screen back when the session ends. A long result of a
# slash command opens a popup over the whole page, and a short one stands above
# the status until the user types. A drag over the transcript copies its text, and
# nothing of the conversation is printed on the terminal's screen after the
# exit. An inline
# session of the same conversation never takes the alternate screen.
set -eu
. "$(dirname "$0")/../harness.sh"

session()
{
    local steps
    steps="$(cat)"
    screen=$1
    rows=${2:-12}
    fyai_test_setup
    mock_start chat_basic_twice.json
    driver=0
    export rows
    {
        cat <<'PTY'
rows: ${rows}
cols: 100
timeout: 20
wait_timeout: 10
steps:
  - type: first question
  - wait: Hello
    timeout: session
PTY
        printf '%s\n' "$steps"
    } | FYAI_TRACE="$TEST_DIR/trace.log" pty_run "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/stream=false \
        --set "display/screen=$screen" \
        ${4:+--set} ${4:+"$4"} \
        --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
        driver=$?
    if grep -a -q "needs a libfytimui" "$TEST_DIR/pty.out" \
            "$TEST_DIR/trace.log" 2>/dev/null; then
        skip "this build has no page support"
    fi
    if [ "$driver" -ne 0 ]; then
        tail -c 2000 "$TEST_DIR/pty.out" >&2
        fail "the $screen session did not go as the case says"
    fi
    mock_stop "${3:-2}"
}

# The view shows both exchanges, goes back to the first with PageUp, and to
# the end with PageDown. The rows of the view, above the blank row and the
# header, hold one exchange and a half: thirteen rows leave six to the view.
# The mock answers at once, so no frame need show the first answer over the
# second question: a wheel step up over the view shows both.
WHEEL=$(printf '\033[<64;10;3M' | od -An -tx1 | tr -d ' \n')
session fullscreen 13 <<'PTY'
  - wait-screen: Hello from the mock provider.
  - type: second question
  - wait-screen: Hello again from the mock provider.
  - wait-gone: "│ first question"
  - write: "\e[<64;10;3M"
  - wait-screen: Hello from the mock provider.
  - write: "\e[5~"
  - wait-screen: "│ first question"
  - write: "\e[6~"
  - wait-gone: "│ first question"
PTY
grep -a -q $'\x1b\[?1049h' "$TEST_DIR/pty.out" ||
    fail "the fullscreen session did not take the alternate screen"
grep -a -q $'\x1b\[?1049l' "$TEST_DIR/pty.out" ||
    fail "the fullscreen session did not give the screen back"
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
data = open(sys.argv[1], "rb").read()
screen = Screen(13, 100)
pos = 0
back = None
both = None
asked = False
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    rows = screen.display()
    # The view is the rows above the blank row that stands over the header.
    head = next((y for y, r in enumerate(rows) if "fyai: session/" in r), None)
    height = head - 1 if head else 6
    view = rows[:height]
    # PageUp takes the view back once the second question was asked; the
    # view is then too short to show both exchanges.
    asked = asked or any("second question" in r for r in rows)
    if asked and any("│ first question" in r for r in view):
        back = rows
        back_height = height
    # A view that shows the first answer and the second question shows no
    # rule between them: the default separator is a blank row.
    ends = [y for y, r in enumerate(view) if "Hello from the mock provider." in r]
    starts = [y for y, r in enumerate(view) if "│ second question" in r]
    if ends and starts and ends[0] < starts[0]:
        both = view
# Back at the first exchange, the view shows it from its top, and the chrome
# stays under the view.
if back is None:
    raise SystemExit("the view never showed the first exchange with the second")
if not any("fyai: session/" in r for r in back[back_height:]):
    raise SystemExit("the chrome did not stay under the view: %r" % back)
if both is None:
    raise SystemExit("the view never showed both exchanges")
ends = [y for y, r in enumerate(both) if "Hello from the mock provider." in r]
starts = [y for y, r in enumerate(both) if "│ second question" in r]
if any("\u2500\u2500\u2500" in r for r in both[ends[0]:starts[0]]):
    raise SystemExit("a rule stands between the exchanges: %r" % both)
PY
    fail "the fullscreen view did not stand above the chrome"

# /help opens a popup over the whole page. A drag over it copies its text and
# PageDown scrolls it; Escape, Enter and a click on the label in its heading
# close it, and each /help after a close shows that the session is still there.
# A drag over five rows of the popup holds text whatever rows of the table wrap.
PDRAG=$(printf '\033[<0;4;4M\033[<32;40;9M\033[<0;40;9m' |
    od -An -tx1 | tr -d ' \n')
# "── log · all · N " takes seventeen columns, so the collapse glyph is on
# column 18.
ACLICK=$(printf '\033[<0;18;1M\033[<0;18;1m' | od -An -tx1 | tr -d ' \n')
session fullscreen 30 1 <<'PTY'
  - wait-screen: Hello from the mock provider.
  - type: "/help"
  - click: "□"
  - wait-screen: Esc collapses
  - write: "\e[<0;4;4M\e[<32;40;9M\e[<0;40;9m"
  - wait-copy: ""
  - write: "\e[6~"
  - wait-gone: "/branches"
  - write: "\e"
  - wait-gone: Esc collapses
  - type: "/help"
  - click: "□"
  - wait-screen: Esc collapses
  - write: "\r"
  - wait-gone: Esc collapses
  - type: "/help"
  - click: "□"
  - wait-screen: Esc collapses
  - write: "\e[<0;18;1M\e[<0;18;1m"
  - wait-gone: Esc collapses
  - write: "\e"
  - wait-gone: "── log"
  - type: "/zoom"
  - wait-screen: nothing is running to zoom into
  - write: "x"
  - wait-gone: nothing is running to zoom into
  - write: "\e"
  - wait-gone: "❯ x"
PTY
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
data = open(sys.argv[1], "rb").read()
# The drag over the popup copied text of the result, not escapes.
terminal = Screen(30, 100)
terminal.feed(data)
if not terminal.clipboard:
    raise SystemExit("a drag over the popup copied nothing")
text = terminal.clipboard[0]
if not text.strip() or "\x1b" in text:
    raise SystemExit("the copy of the popup is not its text: %r" % text)
screen = Screen(30, 100)
pos = 0
popup = scrolled = note = False
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    rows = screen.display()
    if "Esc collapses" in rows[0]:
        popup = True
        # The popup covers the transcript and the chrome.
        for r in rows:
            if "Hello from the mock provider." in r or "fyai: session/" in r:
                raise SystemExit("the page shows beside the popup: %r" % r)
        if "log" not in rows[0]:
            raise SystemExit("the popup does not show the log: %r" %
                             rows[:3])
        if not any("/branches" in r for r in rows[1:]):
            scrolled = True
    at = next((y for y, r in enumerate(rows)
               if "nothing is running to zoom into" in r), None)
    if at is not None:
        note = True
        prompt = next((y for y, r in enumerate(rows) if "❯" in r),
                      None)
        # A short result stands above the status, under the prompt.
        if prompt is None or at < prompt or at > 28 or \
                "Esc collapses" in rows[0]:
            raise SystemExit("the short result is not above the status: %r" %
                             rows)
if not popup:
    raise SystemExit("/help never opened the popup")
if not scrolled:
    raise SystemExit("PageDown did not scroll the popup")
if not note:
    raise SystemExit("the short result never showed")
PY
    fail "the popup and the short result did not show as the case says"

# A popup covers the tiles too, and they come back when it closes. The session
# ends while the shell runs.
session fullscreen 30 1 <<'PTY'
  - wait-screen: Hello from the mock provider.
  - type: "!sh -c 'while :; do printf \"\\r\\033[KSHELLMARK \"; sleep 0.2; done'"
  - wait-screen: SHELLMARK
  - wait-screen: "Ctrl-]"
  - write: "\x1d"
  - wait-gone: "Ctrl-]"
  - type: "/help"
  - click: "□"
  - wait-screen: Esc collapses
  - frame: 3
  - write: "\e"
  - wait-gone: Esc collapses
  - wait-screen: SHELLMARK
PTY
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
data = open(sys.argv[1], "rb").read()
screen = Screen(30, 100)
pos = 0
popup = False
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    rows = screen.display()
    if "Esc collapses" in rows[0]:
        popup = True
        if any("SHELLMARK" in r for r in rows):
            raise SystemExit("the shell shows beside the popup")
if not popup:
    raise SystemExit("/help never opened the popup")
PY
    fail "the popup did not cover the tiles"

# A drag over the answer copies its text with OSC 52, and the session prints
# nothing on the terminal's own screen when it ends.
DRAG=$(printf '\033[<0;3;5M\033[<32;31;5M\033[<0;31;5m' |
    od -An -tx1 | tr -d ' \n')
session fullscreen 30 1 <<'PTY'
  - wait-screen: Hello from the mock provider.
  - write: "\e[<0;3;5M\e[<32;31;5M\e[<0;31;5m"
  - wait-copy: Hello from the mock provider.
PTY
"$PYTHON" - "$TEST_DIR/pty.out" <<'PY' ||
import sys

data = open(sys.argv[1], "rb").read()
left = data.rfind(b"\x1b[?1049l")
if left < 0:
    raise SystemExit("the session never left the alternate screen")
if b"Hello from the mock provider." in data[left:]:
    raise SystemExit("the last exchange was printed after the exit")
PY
    fail "the session printed the conversation after the exit"

# A primary page with tile controls grabs the mouse, so the terminal no longer
# selects. A press and a release over the answer copy its text: the normal
# screen tracks no motion between them.
session primary 30 1 display/work_controls=full <<'PTY'
  - wait-screen: Hello from the mock provider.
  - write: "\e[<0;3;5M\e[<0;31;5m"
  - wait-copy: Hello from the mock provider.
PTY
if grep -a -q "did not take the selection" "$TEST_DIR/pty.out"; then
    fail "the primary page could not copy the selection"
fi

# The same conversation inline stays on the terminal's own screen.
session inline <<'PTY'
  - wait-screen: Hello from the mock provider.
  - type: second question
  - wait-screen: Hello again from the mock provider.
PTY
if grep -a -q $'\x1b\[?1049h' "$TEST_DIR/pty.out"; then
    fail "an inline session took the alternate screen"
fi

pass
