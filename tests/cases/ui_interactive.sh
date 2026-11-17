#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_stream.json

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -b main -k test-key --theme catppuccin:dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
steps:
  - type: hello
  - wait: Streaming hello from the mock.
    timeout: session
PTY

grep -qF "Streaming hello from the mock." "$TEST_DIR/pty.out" || \
    fail "assistant reply was not rendered in the PTY"
"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
    fail "submitted user turn was not committed to the transcript"
import os
import re
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from screen import Screen

data = open(sys.argv[1], "rb").read()
if b"\x1b[38;2;127;132;156m" not in data:
    raise SystemExit("catppuccin status chrome did not reach libfytimui")
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if "│ hello".encode() not in plain:
    raise SystemExit(1)
# The input pane keeps one session row above the prompt block: the
# stored header template, with no duplicated copy beside it. The
# compositor repaints chrome every frame, so the reading is the final
# screen, not the byte stream.
screen = Screen(30, 100)
frames = []
for i in range(0, len(data), 256):
    screen.feed(data[i:i + 256])
    frames.append(list(screen.lines()))
frames.append(list(screen.lines()))
headed = [f for f in frames if any("fyai: main" in l for l in f)]
if not headed:
    raise SystemExit("the input pane header never drew")
# Each frame carries the session row exactly once: a duplicated copy
# beside it would read as a second row naming the session.
for f in headed:
    if sum(l.count("fyai: main") for l in f) != 1:
        raise SystemExit("the input pane header is not a single row")
if not any("mock-model" in l for f in frames for l in f):
    raise SystemExit("the input pane status row lost the model banner")
EOF
assert_request 0 'r["body"]["messages"][-1]["content"] == "hello"'
mock_stop 1

mock_start chat_stream.json
# The session recaps the stored exchange, which holds the same reply: the case
# waits for one more copy of it.
# The case releases the editor when its screen shows that it runs: the editor
# reads a FIFO, and the touch step of the driver opens it for writing, which
# wakes the read. The FIFO is outside the project. The path is in the script:
# the editor does not get the environment of the case.
export MOCK_NOTIFY_DIR
mkfifo "$MOCK_NOTIFY_DIR/release-editor" || fail "cannot make the FIFO"
cat >"$TEST_DIR/editor.sh" <<EOF
#!/bin/sh
printf 'EDITOR-RUNNING\n'
read -r _ <"$MOCK_NOTIFY_DIR/release-editor"
printf 'edited prompt' >"\$1"
EOF
chmod +x "$TEST_DIR/editor.sh"
EDITOR="$TEST_DIR/editor.sh" \
pty_run "$TEST_DIR/edit-line.out" \
    "$FYAI_BIN" -b main -k test-key --theme catppuccin:dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
steps:
  - write: "draft prompt\a"
  - wait: EDITOR-RUNNING
    timeout: 3
  - touch: "${MOCK_NOTIFY_DIR}/release-editor"
  - wait: edited prompt
    timeout: 3
  - write: "\n"
  - wait-count: Streaming hello from the mock.
    more: 1
    timeout: session
PTY
assert_request 0 \
    'r["body"]["messages"][-1]["content"] == "edited prompt"'
mock_stop 1

EDITOR=false \
pty_run "$TEST_DIR/error-pane.out" \
    "$FYAI_BIN" -b main -k test-key --theme catppuccin:dark \
    --set display/markdown=true -m mock-model -i <<'PTY'
exit: {status: 1}
steps:
  - type: "/config edit"
  - wait: editor exited unsuccessfully
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/error-pane.out" <<'EOF' || \
    fail "config editor failure was not rendered in a non-modal error pane"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if "● config".encode() not in plain:
    raise SystemExit("error pane heading missing")
if b"editor exited unsuccessfully" not in plain:
    raise SystemExit("error pane detail missing")
EOF

pty_run "$TEST_DIR/status-pane.out" \
    "$FYAI_BIN" -b main -k test-key --theme catppuccin:dark \
    --set display/markdown=true -m mock-model -i <<'PTY'
steps:
  - type: "/status"
  - wait: "● status"
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/status-pane.out" <<'EOF' || \
    fail "slash-command status was not rendered in a non-modal pane"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if "● status".encode() not in plain or b"Usage / total" not in plain:
    raise SystemExit("status pane content missing")
EOF

pty_run "$TEST_DIR/config-scroll.out" \
    "$FYAI_BIN" -b main -k test-key --theme catppuccin:dark \
    --set display/markdown=true -m mock-model -i <<'PTY'
steps:
  - type: "/config describe display"
  - wait: tool_detail
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/config-scroll.out" <<'EOF' || \
    fail "config report was incorrectly rendered as a status pane"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if "● config".encode() in plain:
    raise SystemExit("config report received status-pane chrome")
if b"tool_detail" not in plain:
    raise SystemExit("config report did not reach terminal scrollback")
EOF

# The first reply holds its stream after the first event until the case has
# seen the line it typed during the turn.
rm -f "$TEST_DIR/release-first"
mock_start chat_stream_queued_input.json
pty_run "$TEST_DIR/queued-input.out" \
    "$FYAI_BIN" -b main -k test-key --theme catppuccin:dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
steps:
  - type: first prompt
  - wait-file: "${TEST_DIR}/served"
    equals: "1"
    timeout: 1.5
  - write: "queued prompt\n"
  - wait: queued prompt
    timeout: 1.5
  - wait-frame: queued prompt
    from: start
    timeout: session
  - touch: "${TEST_DIR}/release-first"
  - wait: Queued input completed.
    timeout: session
PTY
assert_request 0 \
    'r["body"]["messages"][-1]["content"] == "first prompt"'
assert_request 1 \
    'r["body"]["messages"][-1]["content"] == "queued prompt"'
mock_stop 2

pty_run "$TEST_DIR/transcript-order.out" \
    "$FYAI_BIN" -b main -k test-key --theme catppuccin:dark \
    --set display/markdown=true -m mock-model -i <<'PTY'
steps:
  - type: "/transcript all"
  - wait: Queued input completed.
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/transcript-order.out" <<'EOF' || \
    fail "slash transcript did not preserve turn order"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
needles = [
    b"first prompt",
    b"First streamed reply.",
    b"queued prompt",
    b"Queued input completed.",
]
position = 0
for needle in needles:
    position = plain.find(needle, position)
    if position < 0:
        raise SystemExit("slash transcript omitted or reordered %r" % needle)
    position += len(needle)
user_end = plain.find(b"first prompt") + len(b"first prompt")
assistant_start = plain.find(b"First streamed reply.", user_end)
# The card closes its own text row and then draws its bottom card row, which
# is the blank the manager would otherwise add. Two rows stand between the
# words of the card and the answer; a third would be a second blank.
if plain[user_end:assistant_start].count(b"\n") < 2:
    raise SystemExit("slash transcript omitted the bubble's bottom row")
EOF

rm -f "$TEST_DIR/release-first"
mock_start chat_stream_queued_input.json
pty_run "$TEST_DIR/typing-input.out" \
    "$FYAI_BIN" -b main -k test-key --theme catppuccin:dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
exit: {clear: true}
steps:
  - type: first prompt
  - wait-file: "${TEST_DIR}/served"
    equals: "1"
    timeout: 1.5
  - write: "unfinished typing"
  - wait: unfinished typing
    timeout: 1.5
  - wait-frame: unfinished typing
    from: start
    timeout: session
  - touch: "${TEST_DIR}/release-first"
  - wait: First streamed reply.
    timeout: session
PTY
assert_request 0 \
    'r["body"]["messages"][-1]["content"] == "first prompt"'
mock_stop 1

mock_start chat_stream_queued_recall.json
# Give the second queued completion room on a slow runner: the first turn,
# the draft recall, and the resubmitted turn each take the full session
# budget under ASAN with a loaded machine.
# The first turn streams for seconds: the interrupt below must land
# mid-stream whatever the echo latency is. With a short stream a late
# echo flips the choreography to a third request the mock answers
# with a 500, and the needle never comes.
pty_run "$TEST_DIR/interrupt-recall.out" \
    "$FYAI_BIN" -b main -k test-key --theme catppuccin:dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: first prompt
  - wait-file: "${TEST_DIR}/served"
    equals: "1"
  - write: "recalled prompt\n"
  - wait: recalled prompt
    timeout: 1.5
  - write: "\e"
  - wait-count: recalled prompt
    more: 1
    timeout: 1.5
  - wait: interrupted
    timeout: session
  - wait-input: recalled prompt
    timeout: session
  - resubmit: recalled prompt
    expect: Queued input completed.
  - wait: Queued input completed.
    timeout: session
PTY
assert_request 0 \
    'r["body"]["messages"][-1]["content"] == "first prompt"'
assert_request 1 \
    'r["body"]["messages"][-1]["content"] == "recalled prompt"'
mock_stop 2

mock_start chat_stream.json
# Wait for the status row that marks session readiness.
pty_run "$TEST_DIR/blank-input.out" \
    "$FYAI_BIN" -b main -k test-key --theme catppuccin:dark \
    --set display/markdown=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
steps:
  - type: "   "
  - wait: mock-model
    timeout: session
PTY
mock_stop 0

pass
