#!/bin/bash
# SPDX-License-Identifier: MIT
# A line typed while a turn runs waits for the prompt. It is drawn on top of
# the input area, right above its header, in either screen, and as the user
# typed it: its Markdown and tags are text.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
printf 'tool input\n' > data.txt

for screen in inline fullscreen; do
	mock_start chat_tool_pending_user.json
	rm -f "$TEST_DIR/release-1" "$TEST_DIR/served"
	pty_run "$TEST_DIR/$screen.out" \
		"$FYAI_BIN" -b main -k test-key -m mock-model -i --theme dark \
		--set display/markdown=true --set display/screen=$screen \
		--set display/stream=false --set api=chat-completions \
		--set tools=true --set "api_url=$MOCK_URL/v1/chat/completions" <<'PTY'
rows: 30
cols: 100
steps:
  - type: read the file
  - wait-file: "${TEST_DIR}/served"
    equals: "1"
    timeout: 3
  - write: "also *explain* <fy-role name=\"x\">it</fy-role>\n"
  - wait-screen: "pending (1)"
    timeout: 5
  - touch: "${TEST_DIR}/release-1"
  # The second screen recaps the answer that the first one stored, so the
  # answer on the screen does not say that the turn went on: the mock does.
  - wait-file: "${TEST_DIR}/served"
    equals: "2"
    timeout: session
  - wait: I saw the follow-up during the tool loop.
    timeout: session
PTY
	mock_stop 2

	"$PYTHON" - "$TEST_DIR/$screen.out" "$TESTS_DIR" <<'PY' || fail "$screen: the pending line is not on top of the input area"
import sys

sys.path.insert(0, sys.argv[2])
from screen import rows_at

rows = [r.rstrip() for r in rows_at(sys.argv[1], b"pending")]
at = next(i for i, r in enumerate(rows) if "pending (1)" in r)
text = "\u203a also *explain* <fy-role name=\"x\">it</fy-role>"
line = next((i for i, r in enumerate(rows) if text in r), None)
if line is None:
    raise SystemExit("the line is not literal: %r" % rows)
head = next((i for i, r in enumerate(rows) if "fyai: main" in r), None)
if head is None or not at < line < head:
    raise SystemExit("the order is not count, line, header: %r" % rows)
PY
done

# A long queue gives rows up in the block: the oldest lines go, and the count,
# the newest lines, the header and the status stay, with the transcript above.
for screen in inline fullscreen; do
	mock_start chat_tool_pending_user.json
	rm -f "$TEST_DIR/release-1" "$TEST_DIR/served"
	pty_run "$TEST_DIR/long-$screen.out" \
		"$FYAI_BIN" -b main -k test-key -m mock-model -i --theme dark \
		--set display/markdown=true --set display/screen=$screen \
		--set display/stream=false --set api=chat-completions \
		--set tools=true --set "api_url=$MOCK_URL/v1/chat/completions" <<'PTY'
rows: 24
cols: 100
steps:
  - type: read the file
  - wait-file: "${TEST_DIR}/served"
    equals: "1"
    timeout: 3
  - write: "queued 01\nqueued 02\nqueued 03\nqueued 04\nqueued 05\nqueued 06\nqueued 07\nqueued 08\nqueued 09\nqueued 10\nqueued 11\nqueued 12\nqueued 13\nqueued 14\nqueued 15\nqueued 16\nqueued 17\nqueued 18\nqueued 19\nqueued 20\n"
  - wait-screen: "pending (20)"
    timeout: 5
  - wait-screen: "queued 20"
    timeout: 5
  - snapshot: "${TEST_DIR}/long.snap"
  - touch: "${TEST_DIR}/release-1"
  - wait-file: "${TEST_DIR}/served"
    equals: "2"
    timeout: session
  - wait: I saw the follow-up during the tool loop.
    timeout: session
PTY
	mock_stop 2

	"$PYTHON" - "$TEST_DIR/long.snap" "$TESTS_DIR" "$screen" <<'PY' || fail "$screen: a long queue pushes the page off the screen"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

term = Screen(24, 100)
term.feed(open(sys.argv[1], "rb").read())
rows = [r.rstrip() for r in term.display()]


def find(text):
    return next((i for i, r in enumerate(rows) if text in r), None)


count = find("pending (20)")
newest = find("\u203a queued 20")
head = find("fyai: main")
if count is None or newest is None or head is None:
    raise SystemExit("count, newest line or header lost: %r" % rows)
if not count < newest < head:
    raise SystemExit("the order is not count, newest, header: %r" % rows)
if find("\u203a queued 01") is not None:
    raise SystemExit("the oldest line was kept: %r" % rows)
# The input area is under the header, and the status under the input.
if head == len(rows) - 1:
    raise SystemExit("no input area or status under the header: %r" % rows)
if len(rows) - head < 3:
    raise SystemExit("the input area has no room: %r" % rows)
# The block takes a third of the page at most: rows above it stay.
if count < 24 // 3:
    raise SystemExit("the block took the transcript: %r" % rows)
if sys.argv[3] == "fullscreen" and find("read the file") is None:
    raise SystemExit("the transcript region is empty: %r" % rows)
PY
done

pass
