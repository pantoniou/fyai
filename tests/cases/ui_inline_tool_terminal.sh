#!/bin/bash
# SPDX-License-Identifier: MIT
# With display/tool_display=inline, a terminal session of the model draws its
# screen in the transcript of the page ($FYAI_INLINE_SCREEN, fullscreen by
# default), under the head of its call, and not in a tile of the work pane. A
# session that ends leaves its last screen there; a session that runs keeps a live block, whose mark blinks
# while its program writes nothing.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_session.json

# The command row quotes PIPE-B: the output is the row that holds it alone.
FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_INPUT="drive the shell" FYAI_PTY_NEEDLE="done." FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-row:PIPE-B|"\
"wait-screen:● shell [lines]|wait-gone:● shell [lines]|"\
"wait-screen:● shell [lines]" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set "display/screen=${FYAI_INLINE_SCREEN:-fullscreen}" --set display/tool_display=inline \
    --set tools=true --set api=chat-completions \
    --set shell/input_poll_ms=0 \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' || fail "a session screen is not in the transcript"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

# A frame paints only the cells that changed, so a capture cut inside one does
# not show the screen: read the last whole frame that shows the live output.
data = open(sys.argv[1], "rb").read()
rows = []
end = data.find(b"\x1b[?2026l")
while end >= 0:
    s = Screen(30, 100)
    s.feed(data[:end + 8])
    shown = [r.rstrip() for r in s.display() if r.strip()]
    if any(r.strip() == "PIPE-B" for r in shown):
        rows = shown
    end = data.find(b"\x1b[?2026l", end + 1)


def index(pred, what, start=0):
    for i in range(start, len(rows)):
        if pred(rows[i]):
            return i
    raise SystemExit("no %s on screen:\n%s" % (what, "\n".join(rows)))


# The head names the session, or the description alone before the name is
# known: either is the head of the call.
box = index(lambda r: "shell [" in r and "a shell" in r and
            "lines" not in r, "head of the ended session")
said = index(lambda r: r.strip() == "FROM-SESSION", "its output", box)
done = index(lambda r: r.strip() == "done.", "answer", said)
lines = index(lambda r: "shell [lines]" in r, "head of the live session")
index(lambda r: r.strip() == "PIPE-B", "its live output", lines)
if not box < said < done:
    raise SystemExit("the ended session is not at its call:\n" +
                     "\n".join(rows))
PY

grep -q " page: " "$TEST_DIR/trace.log" || fail "the trace has no page"
if grep -E " page: .* tiles=[1-9]" "$TEST_DIR/trace.log" >/dev/null; then
    fail "a session opened a tile of the work pane"
fi

mock_stop 6
pass
